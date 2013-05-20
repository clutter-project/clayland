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

#ifndef __CLAYLAND_POINTER_H__
#define __CLAYLAND_POINTER_H__

#include <wayland-server.h>

#include "clayland-seat.h"

void
clayland_pointer_init (ClaylandPointer *pointer);

void
clayland_pointer_release (ClaylandPointer *pointer);

void
clayland_pointer_set_focus (ClaylandPointer *pointer,
                            ClaylandSurface *surface,
                            wl_fixed_t sx,
                            wl_fixed_t sy);
void
clayland_pointer_start_grab (ClaylandPointer *pointer,
                             ClaylandPointerGrab *grab);

void
clayland_pointer_end_grab (ClaylandPointer *pointer);

void
clayland_pointer_set_current (ClaylandPointer *pointer,
                              ClaylandSurface *surface);

#endif /* __CLAYLAND_POINTER_H__ */
