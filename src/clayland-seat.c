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

#include <clutter/clutter.h>
#include <clutter/wayland/clutter-wayland-compositor.h>
#include <clutter/wayland/clutter-wayland-surface.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include "clayland-seat.h"
#include "clayland-compositor.h"
#include "clayland-keyboard.h"
#include "clayland-pointer.h"
#include "clayland-data-device.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
transform_stage_point_fixed (ClaylandSurface *surface,
                             wl_fixed_t x,
                             wl_fixed_t y,
                             wl_fixed_t *sx,
                             wl_fixed_t *sy)
{
  float xf, yf;

  clutter_actor_transform_stage_point (surface->actor,
                                       wl_fixed_to_double (x),
                                       wl_fixed_to_double (y),
                                       &xf, &yf);

  *sx = wl_fixed_from_double (xf);
  *sy = wl_fixed_from_double (yf);
}

static void
pointer_unmap_sprite (ClaylandSeat *seat)
{
  if (seat->sprite)
    {
      if (seat->sprite->actor)
        clutter_actor_hide (seat->sprite->actor);
      wl_list_remove (&seat->sprite_destroy_listener.link);
      seat->sprite = NULL;
    }
}

static void
pointer_set_cursor (struct wl_client *client,
                    struct wl_resource *resource,
                    uint32_t serial,
                    struct wl_resource *surface_resource,
                    int32_t x, int32_t y)
{
  ClaylandSeat *seat = wl_resource_get_user_data (resource);
  ClaylandSurface *surface;

  surface = (surface_resource ?
             wl_resource_get_user_data (surface_resource) :
             NULL);

  if (seat->pointer.focus == NULL)
    return;
  if (wl_resource_get_client (seat->pointer.focus->resource) != client)
    return;
  if (seat->pointer.focus_serial - serial > G_MAXUINT32 / 2)
    return;

  pointer_unmap_sprite (seat);

  if (!surface)
    return;

  wl_resource_add_destroy_listener (surface->resource,
                                    &seat->sprite_destroy_listener);

  seat->sprite = surface;
  seat->hotspot_x = x;
  seat->hotspot_y = y;
}

static const struct wl_pointer_interface
pointer_interface =
  {
    pointer_set_cursor
  };

static void
seat_get_pointer (struct wl_client *client,
                  struct wl_resource *resource,
                  uint32_t id)
{
  ClaylandSeat *seat = wl_resource_get_user_data (resource);
  struct wl_resource *cr;

  cr = wl_client_add_object (client, &wl_pointer_interface,
                             &pointer_interface, id, seat);
  wl_list_insert (&seat->pointer.resource_list, &cr->link);
  cr->destroy = unbind_resource;

  if (seat->pointer.focus &&
      wl_resource_get_client (seat->pointer.focus->resource) == client)
    {
      ClaylandSurface *surface;
      wl_fixed_t sx, sy;

      surface = (ClaylandSurface *) seat->pointer.focus;
      transform_stage_point_fixed (surface,
                                   seat->pointer.x,
                                   seat->pointer.y,
                                   &sx, &sy);
      clayland_pointer_set_focus (&seat->pointer,
                                  seat->pointer.focus,
                                  sx, sy);
    }
}

static void
seat_get_keyboard (struct wl_client *client,
                   struct wl_resource *resource,
                   uint32_t id)
{
  ClaylandSeat *seat = wl_resource_get_user_data (resource);
  struct wl_resource *cr;

  cr = wl_client_add_object (client, &wl_keyboard_interface, NULL, id, seat);
  wl_list_insert (&seat->keyboard.resource_list, &cr->link);
  cr->destroy = unbind_resource;

  wl_keyboard_send_keymap (cr,
                           WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                           seat->keyboard.xkb_info.keymap_fd,
                           seat->keyboard.xkb_info.keymap_size);

  if (seat->keyboard.focus &&
      wl_resource_get_client (seat->keyboard.focus->resource) == client)
    {
      clayland_keyboard_set_focus (&seat->keyboard,
                                   seat->keyboard.focus);
      clayland_data_device_set_keyboard_focus (seat);
    }
}

static void
seat_get_touch (struct wl_client *client,
                struct wl_resource *resource,
                uint32_t id)
{
  /* Touch not supported */
}

static const struct wl_seat_interface
seat_interface =
  {
    seat_get_pointer,
    seat_get_keyboard,
    seat_get_touch
  };

static void
bind_seat (struct wl_client *client,
           void *data,
           guint32 version,
           guint32 id)
{
  ClaylandSeat *seat = data;
  struct wl_resource *resource;

  resource = wl_client_add_object (client,
                                   &wl_seat_interface,
                                   &seat_interface,
                                   id,
                                   data);
  wl_list_insert (&seat->base_resource_list,
                  wl_resource_get_link (resource));
  wl_resource_set_destructor (resource, unbind_resource);

  wl_seat_send_capabilities (resource,
                             WL_SEAT_CAPABILITY_POINTER |
                             WL_SEAT_CAPABILITY_KEYBOARD);
}

static void
pointer_handle_sprite_destroy (struct wl_listener *listener, void *data)
{
  ClaylandSeat *seat =
    wl_container_of (listener, seat, sprite_destroy_listener);

  seat->sprite = NULL;
}

ClaylandSeat *
clayland_seat_new (struct wl_display *display)
{
  ClaylandSeat *seat = g_new0 (ClaylandSeat, 1);

  wl_signal_init (&seat->destroy_signal);

  seat->selection_data_source = NULL;
  wl_list_init (&seat->base_resource_list);
  wl_signal_init (&seat->selection_signal);
  wl_list_init (&seat->drag_resource_list);
  wl_signal_init (&seat->drag_icon_signal);

  clayland_pointer_init (&seat->pointer);

  clayland_keyboard_init (&seat->keyboard, display);

  seat->display = display;

  seat->current_stage = 0;

  seat->sprite = NULL;
  seat->sprite_destroy_listener.notify = pointer_handle_sprite_destroy;
  seat->hotspot_x = 16;
  seat->hotspot_y = 16;

  wl_display_add_global (display, &wl_seat_interface, seat, bind_seat);

  return seat;
}

static void
notify_motion (ClaylandSeat *seat,
               const ClutterEvent *event)
{
  ClaylandPointer *pointer = &seat->pointer;
  float x, y;

  clutter_event_get_coords (event, &x, &y);
  pointer->x = wl_fixed_from_double (x);
  pointer->y = wl_fixed_from_double (y);

  clayland_seat_repick (seat,
                        clutter_event_get_time (event),
                        clutter_event_get_source (event));

  pointer->grab->interface->motion (pointer->grab,
                                    clutter_event_get_time (event),
                                    pointer->grab->x,
                                    pointer->grab->y);
}

static void
handle_motion_event (ClaylandSeat *seat,
                     const ClutterMotionEvent *event)
{
  notify_motion (seat, (const ClutterEvent *) event);
}

static void
handle_button_event (ClaylandSeat *seat,
                     const ClutterButtonEvent *event)
{
  ClaylandPointer *pointer = &seat->pointer;
  gboolean state = event->type == CLUTTER_BUTTON_PRESS;
  uint32_t button;

  notify_motion (seat, (const ClutterEvent *) event);

  switch (event->button)
    {
      /* The evdev input right and middle button numbers are swapped
         relative to how Clutter numbers them */
    case 2:
      button = BTN_MIDDLE;
      break;

    case 3:
      button = BTN_RIGHT;
      break;

    default:
      button = event->button + BTN_LEFT - 1;
      break;
    }

  if (state)
    {
      if (pointer->button_count == 0)
        {
          pointer->grab_button = button;
          pointer->grab_time = event->time;
          pointer->grab_x = pointer->x;
          pointer->grab_y = pointer->y;
        }

      pointer->button_count++;
    }
  else
    pointer->button_count--;

  pointer->grab->interface->button (pointer->grab, event->time, button, state);

  if (pointer->button_count == 1)
    pointer->grab_serial = wl_display_get_serial (seat->display);
}

void
clayland_seat_handle_event (ClaylandSeat *seat,
                            const ClutterEvent *event)
{
  switch (event->type)
    {
    case CLUTTER_MOTION:
      handle_motion_event (seat,
                           (const ClutterMotionEvent *) event);
      break;

    case CLUTTER_BUTTON_PRESS:
    case CLUTTER_BUTTON_RELEASE:
      handle_button_event (seat,
                           (const ClutterButtonEvent *) event);
      break;

    case CLUTTER_KEY_PRESS:
    case CLUTTER_KEY_RELEASE:
      clayland_keyboard_handle_event (&seat->keyboard,
                                 (const ClutterKeyEvent *) event);
      break;

    default:
      break;
    }
}

/* The actor argument can be NULL in which case a Clutter pick will be
   performed to determine the right actor. An actor should only be
   passed if the repick is being performed due to an event in which
   case Clutter will have already performed a pick so we can avoid
   redundantly doing another one */
void
clayland_seat_repick (ClaylandSeat *seat,
                      uint32_t time,
                      ClutterActor *actor)
{
  ClaylandPointer *pointer = &seat->pointer;
  ClaylandSurface *surface;
  ClaylandSurface *focus;

  if (actor == NULL && seat->current_stage)
    {
      ClutterStage *stage = CLUTTER_STAGE (seat->current_stage);
      actor = clutter_stage_get_actor_at_pos (stage,
                                              CLUTTER_PICK_REACTIVE,
                                              wl_fixed_to_double (pointer->x),
                                              wl_fixed_to_double (pointer->y));
    }

  if (actor)
    seat->current_stage = clutter_actor_get_stage (actor);
  else
    seat->current_stage = NULL;

  if (CLUTTER_WAYLAND_IS_SURFACE (actor))
    {
      ClutterWaylandSurface *wl_surface = CLUTTER_WAYLAND_SURFACE (actor);
      float ax, ay;

      clutter_actor_transform_stage_point (actor,
                                           wl_fixed_to_double (pointer->x),
                                           wl_fixed_to_double (pointer->y),
                                           &ax, &ay);
      pointer->current_x = wl_fixed_from_double (ax);
      pointer->current_y = wl_fixed_from_double (ay);

      surface = ((ClaylandSurface *)
                 clutter_wayland_surface_get_surface (wl_surface));
    }
  else
    surface = NULL;

  if (surface != pointer->current)
    {
      const ClaylandPointerGrabInterface *interface =
        pointer->grab->interface;
      interface->focus (pointer->grab,
                        surface,
                        pointer->current_x, pointer->current_y);
      pointer->current = surface;
    }

  focus = (ClaylandSurface *) pointer->grab->focus;
  if (focus)
    {
      float ax, ay;

      clutter_actor_transform_stage_point (focus->actor,
                                           wl_fixed_to_double (pointer->x),
                                           wl_fixed_to_double (pointer->y),
                                           &ax, &ay);
      pointer->grab->x = wl_fixed_from_double (ax);
      pointer->grab->y = wl_fixed_from_double (ay);
    }
}

void
clayland_seat_free (ClaylandSeat *seat)
{
  pointer_unmap_sprite (seat);

  clayland_pointer_release (&seat->pointer);
  clayland_keyboard_release (&seat->keyboard);

  wl_signal_emit (&seat->destroy_signal, seat);

  g_slice_free (ClaylandSeat, seat);
}
