/*
 * Clayland
 *
 * An example Wayland compositor using Clutter
 *
 * Copyright (C) 2012, 2013  Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#define COGL_ENABLE_EXPERIMENTAL_2_0_API
#include <clutter/clutter.h>
#include <clutter/wayland/clutter-wayland-compositor.h>
#include <clutter/wayland/clutter-wayland-surface.h>

#include <glib.h>
#include <sys/time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <wayland-server.h>

#include "xserver-server-protocol.h"
#include "clayland-compositor.h"
#include "clayland-seat.h"
#include "clayland-data-device.h"
#include "clayland-keyboard.h"

typedef struct
{
  struct wl_resource resource;
  cairo_region_t *region;
} ClaylandRegion;

typedef struct
{
  ClaylandSurface *surface;
  struct wl_resource resource;
  struct wl_listener surface_destroy_listener;
} ClaylandShellSurface;

typedef struct
{
  guint32 flags;
  int width;
  int height;
  int refresh;
} ClaylandMode;

typedef struct
{
  struct wl_object wayland_output;
  int x;
  int y;
  int width_mm;
  int height_mm;
  /* XXX: with sliced stages we'd reference a CoglFramebuffer here. */

  GList *modes;
} ClaylandOutput;

typedef struct
{
  GSource source;
  GPollFD pfd;
  struct wl_display *display;
} WaylandEventSource;

typedef struct
{
  struct wl_list link;

  /* Pointer back to the compositor */
  ClaylandCompositor *compositor;

  struct wl_resource resource;
} ClaylandFrameCallback;

struct _ClaylandCompositor
{
  struct wl_display *wayland_display;
  struct wl_event_loop *wayland_loop;
  ClutterActor *stage;
  GList *outputs;
  GSource *wayland_event_source;
  GList *surfaces;
  struct wl_list frame_callbacks;

  int xwayland_display_index;
  char *xwayland_lockfile;
  int xwayland_abstract_fd;
  int xwayland_unix_fd;
  pid_t xwayland_pid;
  struct wl_client *xwayland_client;
  struct wl_resource *xserver_resource;

  ClaylandSeat *seat;
};

static int signal_pipe[2];

void
report_signal (int signal)
{
  switch (signal)
    {
    case SIGINT:
      write (signal_pipe[1], "I", 1);
      break;
    case SIGCHLD:
      write (signal_pipe[1], "C", 1);
      break;
    default:
      break;
    }
}

static guint32
get_time (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static gboolean
wayland_event_source_prepare (GSource *base, int *timeout)
{
  WaylandEventSource *source = (WaylandEventSource *)base;

  *timeout = -1;

  wl_display_flush_clients (source->display);

  return FALSE;
}

static gboolean
wayland_event_source_check (GSource *base)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  return source->pfd.revents;
}

static gboolean
wayland_event_source_dispatch (GSource *base,
                               GSourceFunc callback,
                               void *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  struct wl_event_loop *loop = wl_display_get_event_loop (source->display);

  wl_event_loop_dispatch (loop, 0);

  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  wayland_event_source_check,
  wayland_event_source_dispatch,
  NULL
};

static GSource *
wayland_event_source_new (struct wl_display *display)
{
  WaylandEventSource *source;
  struct wl_event_loop *loop =
    wl_display_get_event_loop (display);

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->display = display;
  source->pfd.fd = wl_event_loop_get_fd (loop);
  source->pfd.events = G_IO_IN | G_IO_ERR;
  g_source_add_poll (&source->source, &source->pfd);

  return &source->source;
}

static void
surface_damaged (ClaylandSurface *surface,
                 cairo_region_t *region)
{
  struct wl_buffer *wayland_buffer = surface->buffer;

  if (surface->actor)
    {
      int i, n_rectangles = cairo_region_num_rectangles (region);
      ClutterWaylandSurface *surface_actor =
        CLUTTER_WAYLAND_SURFACE (surface->actor);

      for (i = 0; i < n_rectangles; i++)
        {
          cairo_rectangle_int_t rectangle;

          cairo_region_get_rectangle (region, i, &rectangle);

          clutter_wayland_surface_damage_buffer (surface_actor,
                                                 wayland_buffer,
                                                 rectangle.x,
                                                 rectangle.y,
                                                 rectangle.width,
                                                 rectangle.height);
        }
    }
}

static void
clayland_surface_destroy (struct wl_client *wayland_client,
                          struct wl_resource *wayland_resource)
{
  wl_resource_destroy (wayland_resource);
}

static void
clayland_surface_detach_buffer (ClaylandSurface *surface)
{
  struct wl_buffer *buffer = surface->buffer;

  if (buffer)
    {
      wl_list_remove (&surface->buffer_destroy_listener.link);

      surface->buffer = NULL;

      /* FIXME: This should probably ask the ClutterWaylandSurface to
       * destroy its texture? */
    }
}

static void
clayland_surface_detach_buffer_and_notify (ClaylandSurface *surface)
{
  struct wl_buffer *buffer = surface->buffer;

  if (buffer)
    {
      g_assert (buffer->resource.client != NULL);

      wl_resource_queue_event (&buffer->resource, WL_BUFFER_RELEASE);

      clayland_surface_detach_buffer (surface);
    }
}

static void
surface_handle_buffer_destroy (struct wl_listener *listener,
                               void *data)
{
  ClaylandSurface *surface =
    wl_container_of (listener, surface, buffer_destroy_listener);

  clayland_surface_detach_buffer (surface);
}

static void
clayland_surface_attach (struct wl_client *wayland_client,
                         struct wl_resource *wayland_surface_resource,
                         struct wl_resource *wayland_buffer_resource,
                         gint32 sx, gint32 sy)
{
  ClaylandSurface *surface = wayland_surface_resource->data;
  struct wl_buffer *buffer =
    wayland_buffer_resource ? wayland_buffer_resource->data : NULL;

  /* Attach without commit in between does not send wl_buffer.release */
  if (surface->pending.buffer)
    wl_list_remove (&surface->pending.buffer_destroy_listener.link);

  surface->pending.sx = sx;
  surface->pending.sy = sy;
  surface->pending.buffer = buffer;
  surface->pending.newly_attached = TRUE;

  if (buffer)
    wl_signal_add (&buffer->resource.destroy_signal,
                   &surface->pending.buffer_destroy_listener);
}

static void
clayland_surface_damage (struct wl_client *client,
                         struct wl_resource *surface_resource,
                         gint32 x,
                         gint32 y,
                         gint32 width,
                         gint32 height)
{
  ClaylandSurface *surface = surface_resource->data;
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_union_rectangle (surface->pending.damage, &rectangle);
}

static void
destroy_frame_callback (struct wl_resource *callback_resource)
{
  ClaylandFrameCallback *callback = callback_resource->data;

  wl_list_remove (&callback->link);
  g_slice_free (ClaylandFrameCallback, callback);
}

static void
clayland_surface_frame (struct wl_client *client,
                        struct wl_resource *surface_resource,
                        guint32 callback_id)
{
  ClaylandFrameCallback *callback;
  ClaylandSurface *surface = surface_resource->data;

  callback = g_slice_new0 (ClaylandFrameCallback);
  callback->compositor = surface->compositor;
  callback->resource.object.interface = &wl_callback_interface;
  callback->resource.object.id = callback_id;
  callback->resource.destroy = destroy_frame_callback;
  callback->resource.data = callback;

  wl_client_add_resource (client, &callback->resource);
  wl_list_insert (surface->pending.frame_callback_list.prev, &callback->link);
}

static void
clayland_surface_set_opaque_region (struct wl_client *client,
                               struct wl_resource *resource,
                               struct wl_resource *region)
{
}

static void
clayland_surface_set_input_region (struct wl_client *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *region)
{
}

static void
empty_region (cairo_region_t *region)
{
  cairo_rectangle_int_t rectangle = { 0, 0, 0, 0 };
  cairo_region_intersect_rectangle (region, &rectangle);
}

static void
clayland_surface_commit (struct wl_client *client,
                         struct wl_resource *resource)
{
  ClaylandSurface *surface = resource->data;
  ClaylandCompositor *compositor = surface->compositor;

  /* wl_surface.attach */
  if (surface->pending.newly_attached &&
      surface->pending.buffer != surface->buffer)
    {
      clayland_surface_detach_buffer_and_notify (surface);

      if (surface->pending.buffer)
        {
          ClutterWaylandSurface *surface_actor;
          GError *error = NULL;

          if (!surface->actor)
            {
              ClutterActor *stage = compositor->stage;

              surface->actor =
                clutter_wayland_surface_new (&surface->wayland_surface);
              clutter_container_add_actor (CLUTTER_CONTAINER (stage),
                                           surface->actor);
              clutter_actor_set_reactive (surface->actor, TRUE);
            }

          surface_actor = CLUTTER_WAYLAND_SURFACE (surface->actor);

          if (!clutter_wayland_surface_attach_buffer (surface_actor,
                                                      surface->pending.buffer,
                                                      &error))
            {
              g_warning ("Failed to attach buffer to "
                         "ClutterWaylandSurface: %s\n",
                         error->message);
              g_clear_error (&error);
            }

          surface->buffer = surface->pending.buffer;

          wl_signal_add (&surface->buffer->resource.destroy_signal,
                         &surface->buffer_destroy_listener);
        }
    }
  if (surface->pending.buffer)
    {
      wl_list_remove (&surface->pending.buffer_destroy_listener.link);
      surface->pending.buffer = NULL;
    }
  surface->pending.sx = 0;
  surface->pending.sy = 0;
  surface->pending.newly_attached = FALSE;

  /* wl_surface.damage */
  if (surface->buffer &&
      surface->actor)
    surface_damaged (surface, surface->pending.damage);
  empty_region (surface->pending.damage);

  /* wl_surface.frame */
  wl_list_insert_list (&compositor->frame_callbacks,
                       &surface->pending.frame_callback_list);
  wl_list_init (&surface->pending.frame_callback_list);
}

static void
clayland_surface_set_buffer_transform (struct wl_client *client,
                                       struct wl_resource *resource,
                                       int32_t transform)
{
}

const struct wl_surface_interface clayland_surface_interface = {
  clayland_surface_destroy,
  clayland_surface_attach,
  clayland_surface_damage,
  clayland_surface_frame,
  clayland_surface_set_opaque_region,
  clayland_surface_set_input_region,
  clayland_surface_commit,
  clayland_surface_set_buffer_transform
};

/* This should be called whenever the window stacking changes to
   update the current position on all of the input devices */
void
clayland_compositor_repick (ClaylandCompositor *compositor)
{
  clayland_seat_repick (compositor->seat,
                        get_time (),
                        NULL);
}

static void
clayland_surface_free (ClaylandSurface *surface)
{
  ClaylandCompositor *compositor = surface->compositor;
  ClaylandFrameCallback *cb, *next;

  compositor->surfaces = g_list_remove (compositor->surfaces, surface);
  clayland_surface_detach_buffer_and_notify (surface);

  if (surface->actor)
    clutter_actor_destroy (surface->actor);

  if (surface->pending.buffer)
    wl_list_remove (&surface->pending.buffer_destroy_listener.link);

  cairo_region_destroy (surface->pending.damage);

  wl_list_for_each_safe (cb, next,
                         &surface->pending.frame_callback_list, link)
    wl_resource_destroy (&cb->resource);

  g_slice_free (ClaylandSurface, surface);

  clayland_compositor_repick (compositor);
}

static void
clayland_surface_resource_destroy_cb (struct wl_resource *resource)
{
  ClaylandSurface *surface = resource->data;
  clayland_surface_free (surface);
}

static void
surface_handle_pending_buffer_destroy (struct wl_listener *listener,
                                       void *data)
{
  ClaylandSurface *surface =
    wl_container_of (listener, surface, pending.buffer_destroy_listener);

  surface->pending.buffer = NULL;
}

static void
clayland_compositor_create_surface (struct wl_client *wayland_client,
                                    struct wl_resource *compositor_resource,
                                    guint32 id)
{
  ClaylandCompositor *compositor = compositor_resource->data;
  ClaylandSurface *surface = g_slice_new0 (ClaylandSurface);

  surface->compositor = compositor;

  surface->wayland_surface.resource.destroy =
    clayland_surface_resource_destroy_cb;
  surface->wayland_surface.resource.object.id = id;
  surface->wayland_surface.resource.object.interface = &wl_surface_interface;
  surface->wayland_surface.resource.object.implementation =
          (void (**)(void)) &clayland_surface_interface;
  surface->wayland_surface.resource.data = surface;

  surface->pending.damage = cairo_region_create ();

  surface->buffer_destroy_listener.notify =
    surface_handle_buffer_destroy;

  surface->pending.buffer_destroy_listener.notify =
    surface_handle_pending_buffer_destroy;
  wl_list_init (&surface->pending.frame_callback_list);

  wl_client_add_resource (wayland_client, &surface->wayland_surface.resource);

  compositor->surfaces = g_list_prepend (compositor->surfaces, surface);
}

static void
clayland_region_destroy (struct wl_client *client,
                         struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
clayland_region_add (struct wl_client *client,
                     struct wl_resource *resource,
                     gint32 x,
                     gint32 y,
                     gint32 width,
                     gint32 height)
{
  ClaylandRegion *region = resource->data;
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_union_rectangle (region->region, &rectangle);
}

static void
clayland_region_subtract (struct wl_client *client,
                     struct wl_resource *resource,
                     gint32 x,
                     gint32 y,
                     gint32 width,
                     gint32 height)
{
  ClaylandRegion *region = resource->data;
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_subtract_rectangle (region->region, &rectangle);
}

const struct wl_region_interface clayland_region_interface = {
  clayland_region_destroy,
  clayland_region_add,
  clayland_region_subtract
};

static void
clayland_region_resource_destroy_cb (struct wl_resource *resource)
{
  ClaylandRegion *region = resource->data;

  cairo_region_destroy (region->region);
  g_slice_free (ClaylandRegion, region);
}

static void
clayland_compositor_create_region (struct wl_client *wayland_client,
                                   struct wl_resource *compositor_resource,
                                   uint32_t id)
{
  ClaylandRegion *region = g_slice_new0 (ClaylandRegion);

  region->resource.destroy =
    clayland_region_resource_destroy_cb;
  region->resource.object.id = id;
  region->resource.object.interface = &wl_region_interface;
  region->resource.object.implementation =
          (void (**)(void)) &clayland_region_interface;
  region->resource.data = region;

  region->region = cairo_region_create ();

  wl_client_add_resource (wayland_client, &region->resource);
}

static void
bind_output (struct wl_client *client,
             void *data,
             guint32 version,
             guint32 id)
{
  ClaylandOutput *output = data;
  struct wl_resource *resource =
    wl_client_add_object (client, &wl_output_interface, NULL, id, data);
  GList *l;

  wl_resource_post_event (resource,
                          WL_OUTPUT_GEOMETRY,
                          output->x, output->y,
                          output->width_mm,
                          output->height_mm,
                          0, /* subpixel: unknown */
                          "unknown", /* make */
                          "unknown"); /* model */

  for (l = output->modes; l; l = l->next)
    {
      ClaylandMode *mode = l->data;
      wl_resource_post_event (resource,
                              WL_OUTPUT_MODE,
                              mode->flags,
                              mode->width,
                              mode->height,
                              mode->refresh);
    }
}

static void
clayland_compositor_create_output (ClaylandCompositor *compositor,
                                   int x,
                                   int y,
                                   int width_mm,
                                   int height_mm)
{
  ClaylandOutput *output = g_slice_new0 (ClaylandOutput);

  output->wayland_output.interface = &wl_output_interface;

  output->x = x;
  output->y = y;
  output->width_mm = width_mm;
  output->height_mm = height_mm;

  wl_display_add_global (compositor->wayland_display,
                         &wl_output_interface,
                         output,
                         bind_output);

  /* XXX: eventually we will support sliced stages and an output should
   * correspond to a slice/CoglFramebuffer, but for now we only support
   * one output so we make sure it always matches the size of the stage
   */
  clutter_actor_set_size (compositor->stage, width_mm, height_mm);

  compositor->outputs = g_list_prepend (compositor->outputs, output);
}

const static struct wl_compositor_interface clayland_compositor_interface = {
  clayland_compositor_create_surface,
  clayland_compositor_create_region
};

static void
paint_finished_cb (ClutterActor *self, void *user_data)
{
  ClaylandCompositor *compositor = user_data;

  while (!wl_list_empty (&compositor->frame_callbacks))
    {
      ClaylandFrameCallback *callback =
        wl_container_of (compositor->frame_callbacks.next, callback, link);

      wl_resource_post_event (&callback->resource,
                              WL_CALLBACK_DONE, get_time ());
      wl_resource_destroy (&callback->resource);
    }
}

static void
compositor_bind (struct wl_client *client,
		 void *data,
                 guint32 version,
                 guint32 id)
{
  ClaylandCompositor *compositor = data;

  wl_client_add_object (client, &wl_compositor_interface,
                        &clayland_compositor_interface, id, compositor);
}

static void
shell_surface_pong (struct wl_client *client,
                    struct wl_resource *resource,
                    guint32 serial)
{
}

static void
shell_surface_move (struct wl_client *client,
                    struct wl_resource *resource,
                    struct wl_resource *seat,
                    guint32 serial)
{
}

static void
shell_surface_resize (struct wl_client *client,
                      struct wl_resource *resource,
                      struct wl_resource *seat,
                      guint32 serial,
                      guint32 edges)
{
}

static void
shell_surface_set_toplevel (struct wl_client *client,
                            struct wl_resource *resource)
{
}

static void
shell_surface_set_transient (struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *parent,
                             int x,
                             int y,
                             guint32 flags)
{
}

static void
shell_surface_set_fullscreen (struct wl_client *client,
                              struct wl_resource *resource,
                              guint32 method,
                              guint32 framerate,
                              struct wl_resource *output)
{
}

static void
shell_surface_set_popup (struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *seat,
                         guint32 serial,
                         struct wl_resource *parent,
                         gint32 x,
                         gint32 y,
                         guint32 flags)
{
}

static void
shell_surface_set_maximized (struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *output)
{
}

static void
shell_surface_set_title (struct wl_client *client,
                         struct wl_resource *resource,
                         const char *title)
{
}

static void
shell_surface_set_class (struct wl_client *client,
                         struct wl_resource *resource,
                         const char *class_)
{
}

static const struct wl_shell_surface_interface
clayland_shell_surface_interface =
{
  shell_surface_pong,
  shell_surface_move,
  shell_surface_resize,
  shell_surface_set_toplevel,
  shell_surface_set_transient,
  shell_surface_set_fullscreen,
  shell_surface_set_popup,
  shell_surface_set_maximized,
  shell_surface_set_title,
  shell_surface_set_class
};

static void
shell_handle_surface_destroy (struct wl_listener *listener,
                              void *data)
{
  ClaylandShellSurface *shell_surface =
    wl_container_of (listener, shell_surface, surface_destroy_listener);

  shell_surface->surface->has_shell_surface = FALSE;
  shell_surface->surface = NULL;
  wl_resource_destroy (&shell_surface->resource);
}

static void
destroy_shell_surface (struct wl_resource *resource)
{
  ClaylandShellSurface *shell_surface = resource->data;

  /* In case cleaning up a dead client destroys shell_surface first */
  if (shell_surface->surface)
    {
      wl_list_remove (&shell_surface->surface_destroy_listener.link);
      shell_surface->surface->has_shell_surface = FALSE;
    }

  g_free (shell_surface);
}

static void
get_shell_surface (struct wl_client *client,
                   struct wl_resource *resource,
                   guint32 id,
                   struct wl_resource *surface_resource)
{
  ClaylandSurface *surface = surface_resource->data;
  ClaylandShellSurface *shell_surface;

  if (surface->has_shell_surface)
    {
      wl_resource_post_error (surface_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "wl_shell::get_shell_surface already requested");
      return;
    }

  shell_surface = g_new0 (ClaylandShellSurface, 1);
  shell_surface->resource.destroy = destroy_shell_surface;
  shell_surface->resource.object.id = id;
  shell_surface->resource.object.interface = &wl_shell_surface_interface;
  shell_surface->resource.object.implementation =
    (void (**) (void)) &clayland_shell_surface_interface;
  shell_surface->resource.data = shell_surface;

  shell_surface->surface = surface;
  shell_surface->surface_destroy_listener.notify = shell_handle_surface_destroy;
  wl_signal_add (&surface->wayland_surface.resource.destroy_signal,
                 &shell_surface->surface_destroy_listener);

  surface->has_shell_surface = TRUE;

  wl_client_add_resource (client, &shell_surface->resource);
}

static const struct wl_shell_interface clayland_shell_interface =
{
  get_shell_surface
};

static void
bind_shell (struct wl_client *client,
            void *data,
            guint32 version,
            guint32 id)
{
  wl_client_add_object (client, &wl_shell_interface,
                        &clayland_shell_interface, id, data);
}

static char *
create_lockfile (int display, int *display_out)
{
  char *filename;
  int size;
  char pid[11];
  int fd;

  do
    {
      char *end;
      pid_t other;

      filename = g_strdup_printf ("/tmp/.X%d-lock", display);
      fd = open (filename, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0444);

      if (fd < 0 && errno == EEXIST)
        {
          fd = open (filename, O_CLOEXEC, O_RDONLY);
          if (fd < 0 || read (fd, pid, 11) != 11)
            {
              const char *msg = strerror (errno);
              g_warning ("can't read lock file %s: %s", filename, msg);
              g_free (filename);

              /* ignore error and try the next display number */
              display++;
              continue;
          }

          other = strtol (pid, &end, 0);
          if (end != pid + 10)
            {
              g_warning ("can't parse lock file %s", filename);
              g_free (filename);

              /* ignore error and try the next display number */
              display++;
              continue;
          }

          if (kill (other, 0) < 0 && errno == ESRCH)
            {
              g_warning ("unlinking stale lock file %s", filename);
              unlink (filename);
              continue; /* try again */
          }

          g_free (filename);
          display++;
          continue;
        }
      else if (fd < 0)
        {
          const char *msg = strerror (errno);
          g_warning ("failed to create lock file %s: %s", filename , msg);
          g_free (filename);
          return NULL;
        }

      break;
    }
  while (1);

  /* Subtle detail: we use the pid of the wayland compositor, not the xserver
   * in the lock file. */
  size = snprintf (pid, 11, "%10d\n", getpid ());
  if (size != 11 || write (fd, pid, 11) != 11)
    {
      unlink (filename);
      close (fd);
      g_warning ("failed to write pid to lock file %s", filename);
      g_free (filename);
      return NULL;
    }

  close (fd);

  *display_out = display;
  return filename;
}

static int
bind_to_abstract_socket (int display)
{
  struct sockaddr_un addr;
  socklen_t size, name_size;
  int fd;

  fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof addr.sun_path,
                        "%c/tmp/.X11-unix/X%d", 0, display);
  size = offsetof (struct sockaddr_un, sun_path) + name_size;
  if (bind (fd, (struct sockaddr *) &addr, size) < 0)
    {
      g_warning ("failed to bind to @%s: %s\n",
                 addr.sun_path + 1, strerror (errno));
      close (fd);
      return -1;
    }

  if (listen (fd, 1) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

static int
bind_to_unix_socket (int display)
{
  struct sockaddr_un addr;
  socklen_t size, name_size;
  int fd;

  fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof addr.sun_path,
                        "/tmp/.X11-unix/X%d", display) + 1;
  size = offsetof (struct sockaddr_un, sun_path) + name_size;
  unlink (addr.sun_path);
  if (bind (fd, (struct sockaddr *) &addr, size) < 0)
    {
      char *msg = strerror (errno);
      g_warning ("failed to bind to %s (%s)\n", addr.sun_path, msg);
      close (fd);
      return -1;
    }

  if (listen (fd, 1) < 0) {
      unlink (addr.sun_path);
      close (fd);
      return -1;
  }

  return fd;
}

static gboolean
start_xwayland (ClaylandCompositor *compositor)
{
  int display = 0;
  char *lockfile = NULL;
  int sp[2];
  pid_t pid;

  do
    {
      lockfile = create_lockfile (display, &display);
      if (!lockfile)
        {
         g_warning ("Failed to create an X lock file");
         return FALSE;
        }

      compositor->xwayland_abstract_fd = bind_to_abstract_socket (display);
      if (compositor->xwayland_abstract_fd < 0 ||
          compositor->xwayland_abstract_fd == EADDRINUSE)
        {
          unlink (lockfile);
          display++;
          continue;
        }
      compositor->xwayland_unix_fd = bind_to_unix_socket (display);
      if (compositor->xwayland_abstract_fd < 0)
        {
          unlink (lockfile);
          return FALSE;
        }

      break;
    }
  while (1);

  compositor->xwayland_display_index = display;
  compositor->xwayland_lockfile = lockfile;

  /* We want xwayland to be a wayland client so we make a socketpair to setup a
   * wayland protocol connection. */
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) < 0)
    {
      g_warning ("socketpair failed\n");
      unlink (lockfile);
      return 1;
    }

  switch ((pid = fork()))
    {
    case 0:
        {
          char *fd_string;
          char *display_name;
          /* Make sure the client end of the socket pair doesn't get closed
           * when we exec xwayland. */
          int flags = fcntl (sp[1], F_GETFD);
          if (flags != -1)
            fcntl (sp[1], F_SETFD, flags & ~FD_CLOEXEC);

          fd_string = g_strdup_printf ("%d", sp[1]);
          setenv ("WAYLAND_SOCKET", fd_string, 1);
          g_free (fd_string);

          display_name = g_strdup_printf (":%d",
                                          compositor->xwayland_display_index);

          if (execl (XWAYLAND_PATH,
                     XWAYLAND_PATH,
                     display_name,
                     "-wayland",
                     "-rootless",
                     "-retro",
                     /* FIXME: does it make sense to log to the filesystem by
                      * default? */
                     "-logfile", "/tmp/xwayland.log",
                     "-nolisten", "all",
                     "-terminate",
                     NULL) < 0)
            {
              char *msg = strerror (errno);
              g_warning ("xwayland exec failed: %s", msg);
            }
          exit (-1);
          return FALSE;
        }
    default:
      g_message ("forked X server, pid %d\n", pid);

      close (sp[1]);
      compositor->xwayland_client =
        wl_client_create (compositor->wayland_display, sp[0]);

      compositor->xwayland_pid = pid;
      break;

    case -1:
      g_error ("Failed to fork for xwayland server");
      return FALSE;
    }

  return TRUE;
}

static void
stop_xwayland (ClaylandCompositor *compositor)
{
  char path[256];

  snprintf (path, sizeof path, "/tmp/.X%d-lock",
            compositor->xwayland_display_index);
  unlink (path);
  snprintf (path, sizeof path, "/tmp/.X11-unix/X%d",
            compositor->xwayland_display_index);
  unlink (path);

  unlink (compositor->xwayland_lockfile);
}

static void
xserver_set_window_id (struct wl_client *client,
                       struct wl_resource *compositor_resource,
                       struct wl_resource *surface_resource,
                       guint32 id)
{
#if 0
  ClaylandCompositor *compositor = compositor_resource->data;
  struct wlsc_wm *wm = wxs->wm;
  struct wl_surface *surface = surface_resource->data;
  struct wlsc_wm_window *window;

  if (client != wxs->client)
    return;

  window = wl_hash_table_lookup (wm->window_hash, id);
  if (window == NULL)
    {
      g_warning ("set_window_id for unknown window %d", id);
      return;
    }

  g_message ("set_window_id %d for surface %p", id, surface);

  window->surface = (struct wlsc_surface *) surface;
  window->surface_destroy_listener.func = surface_destroy;
  wl_list_insert(surface->resource.destroy_listener_list.prev,
                 &window->surface_destroy_listener.link);
#endif
  g_message ("TODO: xserver_set_window_id");
}

static const struct xserver_interface xserver_implementation = {
    xserver_set_window_id
};

static void
bind_xserver (struct wl_client *client,
	      void *data,
              guint32 version,
              guint32 id)
{
  ClaylandCompositor *compositor = data;

  /* If it's a different client than the xserver we launched,
   * don't start the wm. */
  if (client != compositor->xwayland_client)
    return;

  compositor->xserver_resource =
    wl_client_add_object (client, &xserver_interface,
                          &xserver_implementation, id,
                          compositor);

  /* TODO: Become the window manager for the xserver */

  wl_resource_post_event (compositor->xserver_resource,
                          XSERVER_LISTEN_SOCKET,
                          compositor->xwayland_abstract_fd);

  wl_resource_post_event (compositor->xserver_resource,
                          XSERVER_LISTEN_SOCKET,
                          compositor->xwayland_unix_fd);
  g_warning ("bind_xserver");
}

static gboolean
signal_handler (GIOChannel *source,
                GIOCondition condition,
                void *data)
{
  ClaylandCompositor *compositor = data;
  char signal;
  int count;

  for (;;)
    {
      count = read (signal_pipe[0], &signal, 1);
      if (count == EINTR)
        continue;
      if (count < 0)
        {
          const char *msg = strerror (errno);
          g_warning ("Error handling signal: %s", msg);
        }
      if (count != 1)
        {
          g_warning ("Unexpectedly failed to read byte from signal pipe\n");
          return TRUE;
        }
      break;
    }
  switch (signal)
    {
    case 'I': /* SIGINT */
      /* TODO: cleanup gracefully */
      abort ();
      break;
    case 'C': /* SIGCHLD */
        {
          int status;
          pid_t pid = waitpid (-1, &status, WNOHANG);

          /* The simplest measure to avoid infinitely re-spawning a crashing
           * X server */
          if (!WIFEXITED (status))
              g_critical ("X Wayland crashed; aborting");

          if (pid == compositor->xwayland_pid)
            if (!start_xwayland (compositor))
              g_critical ("Failed to re-start X Wayland server");
        }
      break;
    default:
      g_warning ("Spurious character '%c' read from signal pipe", signal);
    }

  return TRUE;
}

static gboolean
event_cb (ClutterActor *stage,
          const ClutterEvent *event,
          ClaylandCompositor *compositor)
{
  clayland_seat_handle_event (compositor->seat, event);

  /* This implements click-to-focus */
  if (event->type == CLUTTER_BUTTON_PRESS &&
      CLUTTER_WAYLAND_IS_SURFACE (event->any.source))
    {
      ClutterWaylandSurface *cw_surface =
        CLUTTER_WAYLAND_SURFACE (event->any.source);
      struct wl_surface *wl_surface =
        clutter_wayland_surface_get_surface (cw_surface);

      clayland_keyboard_set_focus (&compositor->seat->keyboard, wl_surface);
      clayland_data_device_set_keyboard_focus (compositor->seat);
    }

  return FALSE;
}

int
main (int argc, char **argv)
{
  GIOChannel *signal_reciever;
  struct sigaction signal_action;
  ClaylandCompositor compositor;

  memset (&compositor, 0, sizeof (compositor));

  pipe (signal_pipe);

  signal_reciever = g_io_channel_unix_new (signal_pipe[0]);
  g_io_add_watch (signal_reciever, G_IO_IN, signal_handler, &compositor);

  signal_action.sa_handler = report_signal;
  signal_action.sa_flags = 0;
  sigaction (SIGINT, &signal_action, NULL);
  sigaction (SIGCHLD, &signal_action, NULL);

  compositor.wayland_display = wl_display_create ();
  if (compositor.wayland_display == NULL)
    g_error ("failed to create wayland display");

  wl_list_init (&compositor.frame_callbacks);

  if (!wl_display_add_global (compositor.wayland_display,
                              &wl_compositor_interface,
			      &compositor,
                              compositor_bind))
    g_error ("Failed to register wayland compositor object");

  wl_display_init_shm (compositor.wayland_display);

  compositor.wayland_loop =
    wl_display_get_event_loop (compositor.wayland_display);
  compositor.wayland_event_source =
    wayland_event_source_new (compositor.wayland_display);
  g_source_attach (compositor.wayland_event_source, NULL);

  clutter_wayland_set_compositor_display (compositor.wayland_display);

  if (clutter_init (&argc, &argv) != CLUTTER_INIT_SUCCESS)
    return 1;

  compositor.stage = clutter_stage_new ();
  clutter_stage_set_user_resizable (CLUTTER_STAGE (compositor.stage), FALSE);
  g_signal_connect_after (compositor.stage, "paint",
                          G_CALLBACK (paint_finished_cb), &compositor);

  clayland_data_device_manager_init (compositor.wayland_display);

  compositor.seat = clayland_seat_new (compositor.wayland_display);

  g_signal_connect (compositor.stage,
                    "event",
                    G_CALLBACK (event_cb),
                    &compositor);

  g_signal_connect (compositor.stage,
                    "destroy",
                    G_CALLBACK (clutter_main_quit),
                    NULL /* user_data */);

  clayland_compositor_create_output (&compositor, 0, 0, 800, 600);

  if (wl_display_add_global (compositor.wayland_display, &wl_shell_interface,
                             &compositor, bind_shell) == NULL)
    g_error ("Failed to register a global shell object");

  clutter_actor_show (compositor.stage);

  if (wl_display_add_socket (compositor.wayland_display, "wayland-0"))
    g_error ("Failed to create socket");

  wl_display_add_global (compositor.wayland_display,
                         &xserver_interface,
                         &compositor,
                         bind_xserver);

  /* XXX: It's important that we only try and start xwayland after we have
   * initialized EGL because EGL implements the "wl_drm" interface which
   * xwayland requires to determine what drm device name it should use.
   *
   * By waiting until we've shown the stage above we ensure that the underlying
   * GL resources for the surface have also been allocated and so EGL must be
   * initialized by this point.
   */

  if (!start_xwayland (&compositor))
    return 1;

  clutter_main ();

  stop_xwayland (&compositor);

  /* FIXME: free the input device */

  return 0;
}
