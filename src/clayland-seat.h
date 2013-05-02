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

typedef struct _ClaylandSeat ClaylandSeat;

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
