/*
 * test-wayland-surface
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

#ifndef __TWS_SEAT_H__
#define __TWS_SEAT_H__

#include <wayland-server.h>

typedef struct _TwsSeat TwsSeat;

TwsSeat *
tws_seat_new (struct wl_display *display);

void
tws_seat_handle_event (TwsSeat *seat,
                       const ClutterEvent *event);

void
tws_seat_repick (TwsSeat *seat,
                 uint32_t time,
                 ClutterActor *actor);

void
tws_seat_free (TwsSeat *seat);

#endif /* __TWS_SEAT_H__ */
