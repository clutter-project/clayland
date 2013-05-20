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

#include "clayland-seat.h"

gboolean
clayland_keyboard_init (ClaylandKeyboard *keyboard,
                        struct wl_display *display);

void
clayland_keyboard_handle_event (ClaylandKeyboard *keyboard,
                                const ClutterKeyEvent *event);

void
clayland_keyboard_set_focus (ClaylandKeyboard *keyboard,
                             ClaylandSurface *surface);

void
clayland_keyboard_start_grab (ClaylandKeyboard *device,
                              ClaylandKeyboardGrab *grab);

void
clayland_keyboard_end_grab (ClaylandKeyboard *keyboard);

void
clayland_keyboard_release (ClaylandKeyboard *keyboard);

#endif /* __CLAYLAND_KEYBOARD_H__ */
