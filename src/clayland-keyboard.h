/*
 * Clayland
 *
 * An example Wayland compositor using Clutter
 *
 * Copyright (C) 2013  Intel Corporation.
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

#ifndef __CLAYLAND_KEYBOARD_H__
#define __CLAYLAND_KEYBOARD_H__

#include <clutter/clutter.h>
#include <wayland-server.h>

#include <xkbcommon/xkbcommon.h>

#include "wl-input.h"

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

typedef struct
{
  struct cwl_keyboard parent;

  struct wl_display *display;

  struct xkb_context *xkb_context;

  ClaylandXkbInfo xkb_info;
  struct xkb_rule_names xkb_names;

  struct cwl_keyboard_grab input_method_grab;
  struct wl_resource *input_method_resource;

  ClutterModifierType last_modifier_state;
} ClaylandKeyboard;

gboolean
clayland_keyboard_init (ClaylandKeyboard *keyboard,
                        struct wl_display *display);

void
clayland_keyboard_handle_event (ClaylandKeyboard *keyboard,
                                const ClutterKeyEvent *event);

void
clayland_keyboard_release (ClaylandKeyboard *keyboard);

#endif /* __CLAYLAND_KEYBOARD_H__ */
