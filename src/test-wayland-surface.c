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

typedef struct _TWSCompositor TWSCompositor;

typedef struct
{
  struct wl_buffer *wayland_buffer;
  GList *surfaces_attached_to;
} TWSBuffer;

typedef struct
{
  TWSCompositor *compositor;
  struct wl_surface wayland_surface;
  int x;
  int y;
  TWSBuffer *buffer;
  ClutterActor *actor;
  gboolean has_shell_surface;
} TWSSurface;

typedef struct
{
  TWSSurface *surface;
  struct wl_resource resource;
  struct wl_listener surface_destroy_listener;
} TWSShellSurface;

typedef struct
{
  guint32 flags;
  int width;
  int height;
  int refresh;
} TWSMode;

typedef struct
{
  struct wl_object wayland_output;
  int x;
  int y;
  int width_mm;
  int height_mm;
  /* XXX: with sliced stages we'd reference a CoglFramebuffer here. */

  GList *modes;
} TWSOutput;

typedef struct
{
  GSource source;
  GPollFD pfd;
  struct wl_event_loop *loop;
} WaylandEventSource;

typedef struct
{
  /* GList node used as an embedded list */
  GList node;

  /* Pointer back to the compositor */
  TWSCompositor *compositor;

  struct wl_resource resource;
} TWSFrameCallback;

struct _TWSCompositor
{
  struct wl_display *wayland_display;
  struct wl_shm *wayland_shm;
  struct wl_event_loop *wayland_loop;
  ClutterActor *stage;
  GList *outputs;
  GSource *wayland_event_source;
  GList *surfaces;
  GQueue frame_callbacks;

  int xwayland_display_index;
  char *xwayland_lockfile;
  int xwayland_abstract_fd;
  int xwayland_unix_fd;
  pid_t xwayland_pid;
  struct wl_client *xwayland_client;
  struct wl_resource *xserver_resource;
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
  *timeout = -1;
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
  wl_event_loop_dispatch (source->loop, 0);
  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  wayland_event_source_check,
  wayland_event_source_dispatch,
  NULL
};

GSource *
wayland_event_source_new (struct wl_event_loop *loop)
{
  WaylandEventSource *source;

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->loop = loop;
  source->pfd.fd = wl_event_loop_get_fd (loop);
  source->pfd.events = G_IO_IN | G_IO_ERR;
  g_source_add_poll (&source->source, &source->pfd);

  return &source->source;
}

static TWSBuffer *
tws_buffer_new (struct wl_buffer *wayland_buffer)
{
  TWSBuffer *buffer = g_slice_new (TWSBuffer);

  buffer->wayland_buffer = wayland_buffer;
  buffer->surfaces_attached_to = NULL;

  return buffer;
}

static void
tws_buffer_free (TWSBuffer *buffer)
{
  GList *l;

  buffer->wayland_buffer->user_data = NULL;

  for (l = buffer->surfaces_attached_to; l; l = l->next)
    {
      TWSSurface *surface = l->data;
      surface->buffer = NULL;
    }

  g_list_free (buffer->surfaces_attached_to);
  g_slice_free (TWSBuffer, buffer);
}

static void
shm_buffer_created (struct wl_buffer *wayland_buffer)
{
  wayland_buffer->user_data = tws_buffer_new (wayland_buffer);
}

static void
shm_buffer_damaged (struct wl_buffer *wayland_buffer,
		    gint32 x,
                    gint32 y,
                    gint32 width,
                    gint32 height)
{
  TWSBuffer *buffer = wayland_buffer->user_data;
  GList *l;

  for (l = buffer->surfaces_attached_to; l; l = l->next)
    {
      TWSSurface *surface = l->data;
      ClutterWaylandSurface *surface_actor =
        CLUTTER_WAYLAND_SURFACE (surface->actor);
      clutter_wayland_surface_damage_buffer (surface_actor,
                                             wayland_buffer,
                                             x, y, width, height);
    }
}

static void
shm_buffer_destroyed (struct wl_buffer *wayland_buffer)
{
  if (wayland_buffer->user_data)
    tws_buffer_free ((TWSBuffer *)wayland_buffer->user_data);
}

const static struct wl_shm_callbacks shm_callbacks = {
  shm_buffer_created,
  shm_buffer_damaged,
  shm_buffer_destroyed
};

static void
tws_surface_destroy (struct wl_client *wayland_client,
                     struct wl_resource *wayland_resource)
{
  wl_resource_destroy (wayland_resource, get_time ());
}

static void
tws_surface_detach_buffer (TWSSurface *surface)
{
  TWSBuffer *buffer = surface->buffer;

  if (buffer)
    {
      buffer->surfaces_attached_to =
        g_list_remove (buffer->surfaces_attached_to, surface);
      if (buffer->surfaces_attached_to == NULL)
        tws_buffer_free (buffer);
      surface->buffer = NULL;
    }
}

static void
tws_surface_attach_buffer (struct wl_client *wayland_client,
                           struct wl_resource *wayland_surface_resource,
                           struct wl_resource *wayland_buffer_resource,
                           gint32 dx, gint32 dy)
{
  struct wl_buffer *wayland_buffer = wayland_buffer_resource->data;
  TWSBuffer *buffer = wayland_buffer->user_data;
  TWSSurface *surface = wayland_surface_resource->data;
  TWSCompositor *compositor = surface->compositor;
  ClutterWaylandSurface *surface_actor;

  /* XXX: in the case where we are reattaching the same buffer we can
   * simply bail out. Note this is important because if we don't bail
   * out then the _detach_buffer will actually end up destroying the
   * buffer we're trying to attach. */
  if (buffer && surface->buffer == buffer)
    return;

  tws_surface_detach_buffer (surface);

  /* XXX: we will have been notified of shm buffers already via the
   * callbacks, but this will be the first we know of drm buffers */
  if (!buffer)
    {
      buffer = tws_buffer_new (wayland_buffer);
      wayland_buffer->user_data = buffer;
    }

  g_return_if_fail (g_list_find (buffer->surfaces_attached_to, surface) == NULL);

  buffer->surfaces_attached_to = g_list_prepend (buffer->surfaces_attached_to,
                                                 surface);

  if (!surface->actor)
    {
      surface->actor = clutter_wayland_surface_new (&surface->wayland_surface);
      clutter_container_add_actor (CLUTTER_CONTAINER (compositor->stage),
                                   surface->actor);
    }

  surface_actor = CLUTTER_WAYLAND_SURFACE (surface->actor);
  if (!clutter_wayland_surface_attach_buffer (surface_actor, wayland_buffer,
                                              NULL))
    g_warning ("Failed to attach buffer to ClutterWaylandSurface");

  surface->buffer = buffer;
}

static void
tws_surface_damage (struct wl_client *client,
                    struct wl_resource *resource,
                    gint32 x,
                    gint32 y,
                    gint32 width,
                    gint32 height)
{
}

static void
destroy_frame_callback (struct wl_resource *callback_resource)
{
  TWSFrameCallback *callback = callback_resource->data;

  g_queue_unlink (&callback->compositor->frame_callbacks,
                  &callback->node);

  g_slice_free (TWSFrameCallback, callback);
}

static void
tws_surface_frame (struct wl_client *client,
                   struct wl_resource *surface_resource,
                   guint32 callback_id)
{
  TWSFrameCallback *callback;
  TWSSurface *surface = surface_resource->data;

  callback = g_slice_new0 (TWSFrameCallback);
  callback->compositor = surface->compositor;
  callback->node.data = callback;
  callback->resource.object.interface = &wl_callback_interface;
  callback->resource.object.id = callback_id;
  callback->resource.destroy = destroy_frame_callback;
  callback->resource.data = callback;

  wl_client_add_resource (client, &callback->resource);

  g_queue_push_tail_link (&surface->compositor->frame_callbacks,
                          &callback->node);
}

const struct wl_surface_interface tws_surface_interface = {
  tws_surface_destroy,
  tws_surface_attach_buffer,
  tws_surface_damage,
  tws_surface_frame
};

static void
tws_surface_free (TWSSurface *surface)
{
  TWSCompositor *compositor = surface->compositor;
  compositor->surfaces = g_list_remove (compositor->surfaces, surface);
  tws_surface_detach_buffer (surface);

  if (surface->actor)
    clutter_actor_destroy (surface->actor);

  g_slice_free (TWSSurface, surface);
}

static void
tws_surface_resource_destroy_cb (struct wl_resource *wayland_surface_resource)
{
  TWSSurface *surface = wayland_surface_resource->data;
  tws_surface_free (surface);
}

static void
tws_compositor_create_surface (struct wl_client *wayland_client,
                               struct wl_resource *wayland_compositor_resource,
                               guint32 id)
{
  TWSCompositor *compositor = wayland_compositor_resource->data;
  TWSSurface *surface = g_slice_new0 (TWSSurface);

  surface->compositor = compositor;

  surface->wayland_surface.resource.destroy =
    tws_surface_resource_destroy_cb;
  surface->wayland_surface.resource.object.id = id;
  surface->wayland_surface.resource.object.interface = &wl_surface_interface;
  surface->wayland_surface.resource.object.implementation =
          (void (**)(void)) &tws_surface_interface;
  surface->wayland_surface.resource.data = surface;

  wl_client_add_resource (wayland_client, &surface->wayland_surface.resource);

  compositor->surfaces = g_list_prepend (compositor->surfaces, surface);
}

static void
bind_output (struct wl_client *client,
             void *data,
             guint32 version,
             guint32 id)
{
  TWSOutput *output = data;
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
      TWSMode *mode = l->data;
      wl_resource_post_event (resource,
                              WL_OUTPUT_MODE,
                              mode->flags,
                              mode->width,
                              mode->height,
                              mode->refresh);
    }
}

static void
tws_compositor_create_output (TWSCompositor *compositor,
                              int x,
                              int y,
                              int width_mm,
                              int height_mm)
{
  TWSOutput *output = g_slice_new0 (TWSOutput);

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

const static struct wl_compositor_interface tws_compositor_interface = {
  tws_compositor_create_surface,
};

static void
paint_finished_cb (ClutterActor *self, void *user_data)
{
  TWSCompositor *compositor = user_data;

  while (!g_queue_is_empty (&compositor->frame_callbacks))
    {
      TWSFrameCallback *callback =
        g_queue_peek_head (&compositor->frame_callbacks);

      wl_resource_post_event (&callback->resource,
                              WL_CALLBACK_DONE, get_time ());
      wl_resource_destroy (&callback->resource, 0);
    }
}

static void
compositor_bind (struct wl_client *client,
		 void *data,
                 guint32 version,
                 guint32 id)
{
  TWSCompositor *compositor = data;

  wl_client_add_object (client, &wl_compositor_interface,
                        &tws_compositor_interface, id, compositor);
}

static void
shell_surface_move(struct wl_client *client,
                   struct wl_resource *resource,
                   struct wl_resource *input_resource,
                   guint32 time)
{
}

static void
shell_surface_resize (struct wl_client *client,
                      struct wl_resource *resource,
                      struct wl_resource *input_resource,
                      guint32 time,
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
                             struct wl_resource *parent_resource,
                             int x,
                             int y,
                             guint32 flags)
{
}

static void
shell_surface_set_fullscreen (struct wl_client *client,
                              struct wl_resource *resource)
{
}

static const struct wl_shell_surface_interface tws_shell_surface_interface =
{
  shell_surface_move,
  shell_surface_resize,
  shell_surface_set_toplevel,
  shell_surface_set_transient,
  shell_surface_set_fullscreen
};

static void
shell_handle_surface_destroy (struct wl_listener *listener,
                              struct wl_resource *resource,
                              guint32 time)
{
  TWSShellSurface *shell_surface = container_of (listener,
                                                 TWSShellSurface,
                                                 surface_destroy_listener);

  shell_surface->surface->has_shell_surface = FALSE;
  shell_surface->surface = NULL;
  wl_resource_destroy (&shell_surface->resource, time);
}

static void
destroy_shell_surface (struct wl_resource *resource)
{
  TWSShellSurface *shell_surface = resource->data;

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
  TWSSurface *surface = surface_resource->data;
  TWSShellSurface *shell_surface;

  if (surface->has_shell_surface)
    {
      wl_resource_post_error (surface_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "wl_shell::get_shell_surface already requested");
      return;
    }

  shell_surface = g_new0 (TWSShellSurface, 1);
  shell_surface->resource.destroy = destroy_shell_surface;
  shell_surface->resource.object.id = id;
  shell_surface->resource.object.interface = &wl_shell_surface_interface;
  shell_surface->resource.object.implementation =
    (void (**) (void)) &tws_shell_surface_interface;
  shell_surface->resource.data = shell_surface;

  shell_surface->surface = surface;
  shell_surface->surface_destroy_listener.func = shell_handle_surface_destroy;
  wl_list_insert (surface->wayland_surface.resource.destroy_listener_list.prev,
                  &shell_surface->surface_destroy_listener.link);

  surface->has_shell_surface = TRUE;

  wl_client_add_resource (client, &shell_surface->resource);
}

static const struct wl_shell_interface tws_shell_interface =
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
                        &tws_shell_interface, id, data);
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
start_xwayland (TWSCompositor *compositor)
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

          if (execl ("/home/bob/local/xserver-xwayland/bin/X",
                     "/home/bob/local/xserver-xwayland/bin/X",
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
stop_xwayland (TWSCompositor *compositor)
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
  TWSCompositor *compositor = compositor_resource->data;
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
  TWSCompositor *compositor = data;

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
  TWSCompositor *compositor = data;
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

G_MODULE_EXPORT int
test_wayland_surface_main (int argc, char **argv)
{
  GIOChannel *signal_reciever;
  struct sigaction signal_action;
  TWSCompositor compositor;
  GMainLoop *loop;

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

  g_queue_init (&compositor.frame_callbacks);

  if (!wl_display_add_global (compositor.wayland_display,
                              &wl_compositor_interface,
			      &compositor,
                              compositor_bind))
    g_error ("Failed to register wayland compositor object");

  compositor.wayland_shm = wl_shm_init (compositor.wayland_display,
                                        &shm_callbacks);
  if (!compositor.wayland_shm)
    g_error ("Failed to allocate setup wayland shm callbacks");

  loop = g_main_loop_new (NULL, FALSE);
  compositor.wayland_loop =
    wl_display_get_event_loop (compositor.wayland_display);
  compositor.wayland_event_source =
    wayland_event_source_new (compositor.wayland_loop);
  g_source_attach (compositor.wayland_event_source, NULL);

  clutter_wayland_set_compositor_display (compositor.wayland_display);

  if (clutter_init (&argc, &argv) != CLUTTER_INIT_SUCCESS)
    return 1;

  compositor.stage = clutter_stage_new ();
  clutter_stage_set_user_resizable (CLUTTER_STAGE (compositor.stage), FALSE);
  g_signal_connect_after (compositor.stage, "paint",
                          G_CALLBACK (paint_finished_cb), &compositor);

  tws_compositor_create_output (&compositor, 0, 0, 800, 600);

  if (wl_display_add_global (compositor.wayland_display, &wl_shell_interface,
                             &compositor, bind_shell) == NULL)
    {
      stop_xwayland (&compositor);
      g_error ("Failed to register a global shell object");
    }

  clutter_actor_show (compositor.stage);

  if (wl_display_add_socket (compositor.wayland_display, "wayland-0"))
    {
      stop_xwayland (&compositor);
      g_error ("Failed to create socket");
    }

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


  g_main_loop_run (loop);

  stop_xwayland (&compositor);

  return 0;
}
