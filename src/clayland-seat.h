/*
 * Clayland
 *
 * An example Wayland compositor using Clutter
 *
 * Copyright (C) 2012  Intel Corporation.
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

#ifndef __CLAYLAND_SEAT_H__
#define __CLAYLAND_SEAT_H__

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include <clutter/clutter.h>
#include <glib.h>

#include "clayland-compositor.h"

typedef struct _ClaylandSeat ClaylandSeat;
typedef struct _ClaylandPointer ClaylandPointer;
typedef struct _ClaylandPointerGrab ClaylandPointerGrab;
typedef struct _ClaylandPointerGrabInterface ClaylandPointerGrabInterface;
typedef struct _ClaylandKeyboard ClaylandKeyboard;
typedef struct _ClaylandKeyboardGrab ClaylandKeyboardGrab;
typedef struct _ClaylandKeyboardGrabInterface ClaylandKeyboardGrabInterface;
typedef struct _ClaylandDataOffer ClaylandDataOffer;
typedef struct _ClaylandDataSource ClaylandDataSource;

struct _ClaylandPointerGrabInterface
{
  void (*focus) (ClaylandPointerGrab * grab,
                 ClaylandSurface * surface, wl_fixed_t x, wl_fixed_t y);
  void (*motion) (ClaylandPointerGrab * grab,
                  uint32_t time, wl_fixed_t x, wl_fixed_t y);
  void (*button) (ClaylandPointerGrab * grab,
                  uint32_t time, uint32_t button, uint32_t state);
};

struct _ClaylandPointerGrab
{
  const ClaylandPointerGrabInterface *interface;
  ClaylandPointer *pointer;
  ClaylandSurface *focus;
  wl_fixed_t x, y;
};

struct _ClaylandPointer
{
  struct wl_list resource_list;
  ClaylandSurface *focus;
  struct wl_resource *focus_resource;
  struct wl_listener focus_listener;
  guint32 focus_serial;
  struct wl_signal focus_signal;

  ClaylandPointerGrab *grab;
  ClaylandPointerGrab default_grab;
  wl_fixed_t grab_x, grab_y;
  guint32 grab_button;
  guint32 grab_serial;
  guint32 grab_time;

  wl_fixed_t x, y;
  ClaylandSurface *current;
  struct wl_listener current_listener;
  wl_fixed_t current_x, current_y;

  guint32 button_count;
};

struct _ClaylandKeyboardGrabInterface
{
  void (*key) (ClaylandKeyboardGrab * grab, uint32_t time,
               uint32_t key, uint32_t state);
  void (*modifiers) (ClaylandKeyboardGrab * grab, uint32_t serial,
                     uint32_t mods_depressed, uint32_t mods_latched,
                     uint32_t mods_locked, uint32_t group);
};

struct _ClaylandKeyboardGrab
{
  const ClaylandKeyboardGrabInterface *interface;
  ClaylandKeyboard *keyboard;
  ClaylandSurface *focus;
  uint32_t key;
};

typedef struct
{
  struct xkb_keymap *keymap;
  int keymap_fd;
  size_t keymap_size;
  char *keymap_area;
  xkb_mod_index_t shift_mod;
  xkb_mod_index_t caps_mod;
  xkb_mod_index_t ctrl_mod;
  xkb_mod_index_t alt_mod;
  xkb_mod_index_t mod2_mod;
  xkb_mod_index_t mod3_mod;
  xkb_mod_index_t super_mod;
  xkb_mod_index_t mod5_mod;
} ClaylandXkbInfo;

struct _ClaylandKeyboard
{
  struct wl_list resource_list;
  ClaylandSurface *focus;
  struct wl_resource *focus_resource;
  struct wl_listener focus_listener;
  uint32_t focus_serial;
  struct wl_signal focus_signal;

  ClaylandKeyboardGrab *grab;
  ClaylandKeyboardGrab default_grab;
  uint32_t grab_key;
  uint32_t grab_serial;
  uint32_t grab_time;

  struct wl_array keys;

  struct
  {
    uint32_t mods_depressed;
    uint32_t mods_latched;
    uint32_t mods_locked;
    uint32_t group;
  } modifiers;

  struct wl_display *display;

  struct xkb_context *xkb_context;

  ClaylandXkbInfo xkb_info;
  struct xkb_rule_names xkb_names;

  ClaylandKeyboardGrab input_method_grab;
  struct wl_resource *input_method_resource;

  ClutterModifierType last_modifier_state;
};

struct _ClaylandDataOffer
{
  struct wl_resource *resource;
  ClaylandDataSource *source;
  struct wl_listener source_destroy_listener;
};

struct _ClaylandDataSource
{
  struct wl_resource *resource;
  struct wl_signal destroy_signal;
  struct wl_array mime_types;

  void (*accept) (ClaylandDataSource * source,
                  uint32_t serial, const char *mime_type);
  void (*send) (ClaylandDataSource * source,
                const char *mime_type, int32_t fd);
  void (*cancel) (ClaylandDataSource * source);
};

struct _ClaylandSeat
{
  struct wl_list base_resource_list;
  struct wl_signal destroy_signal;

  uint32_t selection_serial;
  ClaylandDataSource *selection_data_source;
  struct wl_listener selection_data_source_listener;
  struct wl_signal selection_signal;

  struct wl_list drag_resource_list;
  struct wl_client *drag_client;
  ClaylandDataSource *drag_data_source;
  struct wl_listener drag_data_source_listener;
  ClaylandSurface *drag_focus;
  struct wl_resource *drag_focus_resource;
  struct wl_listener drag_focus_listener;
  ClaylandPointerGrab drag_grab;
  ClaylandSurface *drag_surface;
  struct wl_listener drag_icon_listener;
  struct wl_signal drag_icon_signal;

  ClaylandPointer pointer;
  ClaylandKeyboard keyboard;

  struct wl_display *display;

  ClaylandSurface *sprite;
  int hotspot_x, hotspot_y;
  struct wl_listener sprite_destroy_listener;

  ClutterActor *current_stage;
};

ClaylandSeat *
clayland_seat_new (struct wl_display *display);

void
clayland_seat_handle_event (ClaylandSeat *seat,
                            const ClutterEvent *event);

void
clayland_seat_repick (ClaylandSeat *seat,
                      uint32_t time,
                      ClutterActor *actor);

void
clayland_seat_free (ClaylandSeat *seat);

#endif /* __CLAYLAND_SEAT_H__ */
