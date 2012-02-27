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

#ifndef __TWS_INPUT_H__
#define __TWS_INPUT_H__

#include <wayland-server.h>

typedef struct _TwsInputDevice TwsInputDevice;

TwsInputDevice *
tws_input_device_new (struct wl_display *display);

void
tws_input_device_handle_event (TwsInputDevice *input_device,
                               const ClutterEvent *event);

void
tws_input_device_repick (TwsInputDevice *input_device,
                         uint32_t time,
                         ClutterActor *actor);

#endif /* __TWS_INPUT_H__ */
