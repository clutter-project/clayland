/*
 * Copyright © 2011 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#include "clayland-data-device.h"
#include "clayland-seat.h"
#include "clayland-pointer.h"

static void
data_offer_accept (struct wl_client *client,
                   struct wl_resource *resource,
                   guint32 serial,
                   const char *mime_type)
{
  ClaylandDataOffer *offer = resource->data;

  /* FIXME: Check that client is currently focused by the input
   * device that is currently dragging this data source.  Should
   * this be a wl_data_device request? */

  if (offer->source)
    offer->source->accept (offer->source, serial, mime_type);
}

static void
data_offer_receive (struct wl_client *client, struct wl_resource *resource,
                    const char *mime_type, int32_t fd)
{
  ClaylandDataOffer *offer = resource->data;

  if (offer->source)
    offer->source->send (offer->source, mime_type, fd);
  else
    close (fd);
}

static void
data_offer_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_data_offer_interface data_offer_interface = {
  data_offer_accept,
  data_offer_receive,
  data_offer_destroy,
};

static void
destroy_data_offer (struct wl_resource *resource)
{
  ClaylandDataOffer *offer = resource->data;

  if (offer->source)
    wl_list_remove (&offer->source_destroy_listener.link);
  free (offer);
}

static void
destroy_offer_data_source (struct wl_listener *listener, void *data)
{
  ClaylandDataOffer *offer;

  offer = wl_container_of (listener, offer, source_destroy_listener);

  offer->source = NULL;
}

static struct wl_resource *
clayland_data_source_send_offer (ClaylandDataSource *source,
                                 struct wl_resource *target)
{
  ClaylandDataOffer *offer;
  char **p;

  offer = malloc (sizeof *offer);
  if (offer == NULL)
    return NULL;

  wl_resource_init (&offer->resource, &wl_data_offer_interface,
                    &data_offer_interface, 0, offer);
  offer->resource.destroy = destroy_data_offer;

  offer->source = source;
  offer->source_destroy_listener.notify = destroy_offer_data_source;
  wl_signal_add (&source->resource.destroy_signal,
                 &offer->source_destroy_listener);

  wl_client_add_resource (target->client, &offer->resource);

  wl_data_device_send_data_offer (target, &offer->resource);

  wl_array_for_each (p, &source->mime_types)
    wl_data_offer_send_offer (&offer->resource, *p);

  return &offer->resource;
}

static void
data_source_offer (struct wl_client *client,
                   struct wl_resource *resource, const char *type)
{
  ClaylandDataSource *source = resource->data;
  char **p;

  p = wl_array_add (&source->mime_types, sizeof *p);
  if (p)
    *p = strdup (type);
  if (!p || !*p)
    wl_resource_post_no_memory (resource);
}

static void
data_source_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct wl_data_source_interface data_source_interface = {
  data_source_offer,
  data_source_destroy
};

static struct wl_resource *
find_resource (struct wl_list *list, struct wl_client *client)
{
  struct wl_resource *r;

  wl_list_for_each (r, list, link)
  {
    if (r->client == client)
      return r;
  }

  return NULL;
}

static void
destroy_drag_focus (struct wl_listener *listener, void *data)
{
  ClaylandSeat *seat = wl_container_of (listener, seat, drag_focus_listener);

  seat->drag_focus_resource = NULL;
}

static void
drag_grab_focus (ClaylandPointerGrab *grab,
                 struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
  ClaylandSeat *seat = wl_container_of (grab, seat, drag_grab);
  struct wl_resource *resource, *offer = NULL;
  struct wl_display *display;
  guint32 serial;

  if (seat->drag_focus_resource)
    {
      wl_data_device_send_leave (seat->drag_focus_resource);
      wl_list_remove (&seat->drag_focus_listener.link);
      seat->drag_focus_resource = NULL;
      seat->drag_focus = NULL;
    }

  if (!surface)
    return;

  if (!seat->drag_data_source &&
      surface->resource.client != seat->drag_client)
    return;

  resource = find_resource (&seat->drag_resource_list,
                            surface->resource.client);
  if (!resource)
    return;

  display = wl_client_get_display (resource->client);
  serial = wl_display_next_serial (display);

  if (seat->drag_data_source)
    offer = clayland_data_source_send_offer (seat->drag_data_source, resource);

  wl_data_device_send_enter (resource, serial, &surface->resource,
                             x, y, offer);

  seat->drag_focus = surface;
  seat->drag_focus_listener.notify = destroy_drag_focus;
  wl_signal_add (&resource->destroy_signal, &seat->drag_focus_listener);
  seat->drag_focus_resource = resource;
  grab->focus = surface;
}

static void
drag_grab_motion (ClaylandPointerGrab *grab,
                  guint32 time, wl_fixed_t x, wl_fixed_t y)
{
  ClaylandSeat *seat = wl_container_of (grab, seat, drag_grab);

  if (seat->drag_focus_resource)
    wl_data_device_send_motion (seat->drag_focus_resource, time, x, y);
}

static void
data_device_end_drag_grab (ClaylandSeat *seat)
{
  if (seat->drag_surface)
    {
      seat->drag_surface = NULL;
      wl_signal_emit (&seat->drag_icon_signal, NULL);
      wl_list_remove (&seat->drag_icon_listener.link);
    }

  drag_grab_focus (&seat->drag_grab, NULL,
                   wl_fixed_from_int (0), wl_fixed_from_int (0));

  clayland_pointer_end_grab (&seat->pointer);

  seat->drag_data_source = NULL;
  seat->drag_client = NULL;
}

static void
drag_grab_button (ClaylandPointerGrab *grab,
                  guint32 time, guint32 button, guint32 state_w)
{
  ClaylandSeat *seat = wl_container_of (grab, seat, drag_grab);
  enum wl_pointer_button_state state = state_w;

  if (seat->drag_focus_resource &&
      seat->pointer.grab_button == button &&
      state == WL_POINTER_BUTTON_STATE_RELEASED)
    wl_data_device_send_drop (seat->drag_focus_resource);

  if (seat->pointer.button_count == 0 &&
      state == WL_POINTER_BUTTON_STATE_RELEASED)
    {
      if (seat->drag_data_source)
        wl_list_remove (&seat->drag_data_source_listener.link);
      data_device_end_drag_grab (seat);
    }
}

static const ClaylandPointerGrabInterface drag_grab_interface = {
  drag_grab_focus,
  drag_grab_motion,
  drag_grab_button,
};

static void
destroy_data_device_source (struct wl_listener *listener, void *data)
{
  ClaylandSeat *seat =
    wl_container_of (listener, seat, drag_data_source_listener);

  data_device_end_drag_grab (seat);
}

static void
destroy_data_device_icon (struct wl_listener *listener, void *data)
{
  ClaylandSeat *seat =
    wl_container_of (listener, seat, drag_icon_listener);

  seat->drag_surface = NULL;
}

static void
data_device_start_drag (struct wl_client *client,
                        struct wl_resource *resource,
                        struct wl_resource *source_resource,
                        struct wl_resource *origin_resource,
                        struct wl_resource *icon_resource, guint32 serial)
{
  ClaylandSeat *seat = resource->data;

  /* FIXME: Check that client has implicit grab on the origin
   * surface that matches the given time. */

  /* FIXME: Check that the data source type array isn't empty. */

  seat->drag_grab.interface = &drag_grab_interface;

  seat->drag_client = client;
  seat->drag_data_source = NULL;

  if (source_resource)
    {
      seat->drag_data_source = source_resource->data;
      seat->drag_data_source_listener.notify = destroy_data_device_source;
      wl_signal_add (&source_resource->destroy_signal,
                     &seat->drag_data_source_listener);
    }

  if (icon_resource)
    {
      seat->drag_surface = icon_resource->data;
      seat->drag_icon_listener.notify = destroy_data_device_icon;
      wl_signal_add (&icon_resource->destroy_signal,
                     &seat->drag_icon_listener);
      wl_signal_emit (&seat->drag_icon_signal, icon_resource);
    }

  clayland_pointer_set_focus (&seat->pointer, NULL,
                              wl_fixed_from_int (0),
                              wl_fixed_from_int (0));
  clayland_pointer_start_grab (&seat->pointer, &seat->drag_grab);
}

static void
destroy_selection_data_source (struct wl_listener *listener, void *data)
{
  ClaylandSeat *seat =
    wl_container_of (listener, seat, selection_data_source_listener);
  struct wl_resource *data_device;
  struct wl_resource *focus = NULL;

  seat->selection_data_source = NULL;

  focus = seat->keyboard.focus_resource;

  if (focus)
    {
      data_device = find_resource (&seat->drag_resource_list, focus->client);
      if (data_device)
        wl_data_device_send_selection (data_device, NULL);
    }

  wl_signal_emit (&seat->selection_signal, seat);
}

void
cwl_seat_set_selection (ClaylandSeat *seat, ClaylandDataSource *source,
                        guint32 serial)
{
  struct wl_resource *data_device, *offer;
  struct wl_resource *focus = NULL;

  if (seat->selection_data_source &&
      seat->selection_serial - serial < UINT32_MAX / 2)
    return;

  if (seat->selection_data_source)
    {
      seat->selection_data_source->cancel (seat->selection_data_source);
      wl_list_remove (&seat->selection_data_source_listener.link);
      seat->selection_data_source = NULL;
    }

  seat->selection_data_source = source;
  seat->selection_serial = serial;

  focus = seat->keyboard.focus_resource;

  if (focus)
    {
      data_device = find_resource (&seat->drag_resource_list, focus->client);
      if (data_device && source)
        {
          offer = clayland_data_source_send_offer (seat->selection_data_source,
                                                   data_device);
          wl_data_device_send_selection (data_device, offer);
        }
      else if (data_device)
        {
          wl_data_device_send_selection (data_device, NULL);
        }
    }

  wl_signal_emit (&seat->selection_signal, seat);

  if (source)
    {
      seat->selection_data_source_listener.notify =
        destroy_selection_data_source;
      wl_signal_add (&source->resource.destroy_signal,
                     &seat->selection_data_source_listener);
    }
}

static void
data_device_set_selection (struct wl_client *client,
                           struct wl_resource *resource,
                           struct wl_resource *source_resource,
                           guint32 serial)
{
  if (!source_resource)
    return;

  /* FIXME: Store serial and check against incoming serial here. */
  cwl_seat_set_selection (resource->data, source_resource->data, serial);
}

static const struct wl_data_device_interface data_device_interface = {
  data_device_start_drag,
  data_device_set_selection,
};

static void
destroy_data_source (struct wl_resource *resource)
{
  ClaylandDataSource *source = wl_container_of (resource, source, resource);
  char **p;

  wl_array_for_each (p, &source->mime_types) free (*p);

  wl_array_release (&source->mime_types);

  source->resource.object.id = 0;
}

static void
client_source_accept (ClaylandDataSource *source,
                      guint32 time, const char *mime_type)
{
  wl_data_source_send_target (&source->resource, mime_type);
}

static void
client_source_send (ClaylandDataSource *source,
                    const char *mime_type, int32_t fd)
{
  wl_data_source_send_send (&source->resource, mime_type, fd);
  close (fd);
}

static void
client_source_cancel (ClaylandDataSource *source)
{
  wl_data_source_send_cancelled (&source->resource);
}

static void
create_data_source (struct wl_client *client,
                    struct wl_resource *resource, guint32 id)
{
  ClaylandDataSource *source;

  source = malloc (sizeof *source);
  if (source == NULL)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_init (&source->resource, &wl_data_source_interface,
                    &data_source_interface, id, source);
  source->resource.destroy = destroy_data_source;

  source->accept = client_source_accept;
  source->send = client_source_send;
  source->cancel = client_source_cancel;

  wl_array_init (&source->mime_types);
  wl_client_add_resource (client, &source->resource);
}

static void
unbind_data_device (struct wl_resource *resource)
{
  wl_list_remove (&resource->link);
  free (resource);
}

static void
get_data_device (struct wl_client *client,
                 struct wl_resource *manager_resource,
                 guint32 id, struct wl_resource *seat_resource)
{
  ClaylandSeat *seat = seat_resource->data;
  struct wl_resource *resource;

  resource = wl_client_add_object (client, &wl_data_device_interface,
                                   &data_device_interface, id, seat);

  wl_list_insert (&seat->drag_resource_list, &resource->link);
  resource->destroy = unbind_data_device;
}

static const struct wl_data_device_manager_interface manager_interface = {
  create_data_source,
  get_data_device
};

static void
bind_manager (struct wl_client *client,
              void *data, guint32 version, guint32 id)
{
  wl_client_add_object (client, &wl_data_device_manager_interface,
                        &manager_interface, id, NULL);
}

void
clayland_data_device_set_keyboard_focus (ClaylandSeat *seat)
{
  struct wl_resource *data_device, *focus, *offer;
  ClaylandDataSource *source;

  focus = seat->keyboard.focus_resource;
  if (!focus)
    return;

  data_device = find_resource (&seat->drag_resource_list, focus->client);
  if (!data_device)
    return;

  source = seat->selection_data_source;
  if (source)
    {
      offer = clayland_data_source_send_offer (source, data_device);
      wl_data_device_send_selection (data_device, offer);
    }
}

int
clayland_data_device_manager_init (struct wl_display *display)
{
  if (wl_display_add_global (display,
                             &wl_data_device_manager_interface,
                             NULL, bind_manager) == NULL)
    return -1;

  return 0;
}
