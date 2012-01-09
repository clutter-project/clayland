#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <clutter/clutter.h>
#include <clutter/wayland/clutter-wayland-compositor.h>
#include <clutter/wayland/clutter-wayland-surface.h>
#include <stdlib.h>
#include "tws-input.h"
#include "tws-compositor.h"

struct _TwsInputDevice
{
  struct wl_input_device parent;

  /* Last position of the pointer */
  float pointer_x, pointer_y;

  /* Position of the pointer within the surface */
  int32_t pointer_sx, pointer_sy;
};

static void
implicit_grab_motion (struct wl_grab *grab,
                      uint32_t time,
                      int32_t x,
                      int32_t y)
{
  struct wl_resource *resource;

  resource = grab->input_device->pointer_focus_resource;

  if (resource)
    {
      TWSSurface *surface = (TWSSurface *) grab->input_device->pointer_focus;
      float sx, sy;

      clutter_actor_transform_stage_point (surface->actor,
                                           x,
                                           y,
                                           &sx,
                                           &sy);

      wl_resource_post_event (resource, WL_INPUT_DEVICE_MOTION,
                              time, x, y, (int32_t) sx, (int32_t) sy);
    }
}

static void
implicit_grab_button (struct wl_grab *grab,
                      uint32_t time,
                      int32_t button,
                      int32_t state)
{
  struct wl_resource *resource;

  resource = grab->input_device->pointer_focus_resource;

  if (resource)
    wl_resource_post_event (resource, WL_INPUT_DEVICE_BUTTON,
                            time, button, state);
}

static void
implicit_grab_end(struct wl_grab *grab, uint32_t time)
{
}

static const struct wl_grab_interface implicit_grab_interface = {
  implicit_grab_motion,
  implicit_grab_button,
  implicit_grab_end
};

static void
input_device_attach (struct wl_client *client,
                     struct wl_resource *resource,
                     uint32_t time,
                     struct wl_resource *buffer_resource,
                     int32_t hotspot_x,
                     int32_t hotspot_y)
{
}

const static struct wl_input_device_interface
input_device_interface =
  {
    input_device_attach
  };

static void
unbind_input_device (struct wl_resource *resource)
{
  wl_list_remove (&resource->link);
  free (resource);
}

static void
bind_input_device (struct wl_client *client,
                   void *data,
                   uint32_t version,
                   uint32_t id)
{
  struct wl_input_device *device = data;
  struct wl_resource *resource;

  resource = wl_client_add_object (client,
                                   &wl_input_device_interface,
                                   &input_device_interface,
                                   id,
                                   data);

  wl_list_insert (&device->resource_list, &resource->link);

  resource->destroy = unbind_input_device;
}

TwsInputDevice *
tws_input_device_new (struct wl_display *display)
{
  TwsInputDevice *device = g_new (TwsInputDevice, 1);

  wl_input_device_init (&device->parent);

  device->parent.implicit_grab.interface = &implicit_grab_interface;

  wl_display_add_global (display,
                         &wl_input_device_interface,
                         device,
                         bind_input_device);

  return device;
}

static void
set_pointer_focus_for_event (TwsInputDevice *input_device,
                             const ClutterEvent *event)
{
  struct wl_input_device *device =
    (struct wl_input_device *) input_device;
  struct wl_surface *surface;

  clutter_event_get_coords (event,
                            &input_device->pointer_x,
                            &input_device->pointer_y);

  if (CLUTTER_WAYLAND_IS_SURFACE (event->any.source))
    {
      ClutterWaylandSurface *surface_actor =
        CLUTTER_WAYLAND_SURFACE (event->any.source);
      float fsx, fsy;

      surface = clutter_wayland_surface_get_surface (surface_actor);

      clutter_actor_transform_stage_point (event->any.source,
                                           input_device->pointer_x,
                                           input_device->pointer_y,
                                           &fsx, &fsy);
      input_device->pointer_sx = fsx;
      input_device->pointer_sy = fsy;
    }
  else
    {
      surface = NULL;
      input_device->pointer_sx = input_device->pointer_x;
      input_device->pointer_sy = input_device->pointer_y;
    }

  wl_input_device_set_pointer_focus (device,
                                     surface,
                                     event->any.time,
                                     input_device->pointer_x,
                                     input_device->pointer_y,
                                     input_device->pointer_sx,
                                     input_device->pointer_sy);
}

static void
handle_motion_event (TwsInputDevice *input_device,
                     const ClutterMotionEvent *event)
{
  struct wl_input_device *device =
    (struct wl_input_device *) input_device;

  if (device->grab)
    device->grab->interface->motion (device->grab,
                                     event->time,
                                     event->x,
                                     event->y);
  else
    {
      set_pointer_focus_for_event (input_device, (const ClutterEvent *) event);

      if (device->pointer_focus_resource)
        wl_resource_post_event (device->pointer_focus_resource,
                                WL_INPUT_DEVICE_MOTION,
                                time,
                                (int32_t) input_device->pointer_x,
                                (int32_t) input_device->pointer_y,
                                input_device->pointer_sx,
                                input_device->pointer_sy);
    }
}

static void
handle_button_press_event (TwsInputDevice *input_device,
                           const ClutterButtonEvent *event)
{
  struct wl_input_device *device =
    (struct wl_input_device *) input_device;
  struct wl_surface *surface = device->pointer_focus;
  gboolean state = event->type == CLUTTER_BUTTON_PRESS;

  if (state && surface && device->grab == NULL)
    wl_input_device_start_grab (device,
                                &device->implicit_grab,
                                event->button,
                                event->time);

  if (device->grab)
    device->grab->interface->button (device->grab,
                                     event->time,
                                     event->button,
                                     state);

  if (!state && device->grab && device->grab_button == event->button)
    {
      wl_input_device_end_grab (device, event->time);

      set_pointer_focus_for_event (input_device, (const ClutterEvent *) event);
    }
}

void
tws_input_device_handle_event (TwsInputDevice *input_device,
                               const ClutterEvent *event)
{
  switch (event->type)
    {
    case CLUTTER_MOTION:
      handle_motion_event (input_device,
                           (const ClutterMotionEvent *) event);
      break;

    case CLUTTER_BUTTON_PRESS:
      handle_button_press_event (input_device,
                                 (const ClutterButtonEvent *) event);
      break;

    default:
      break;
    }
}
