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

#ifndef __TWS_COMPOSITOR_H__
#define __TWS_COMPOSITOR_H__

#include <glib.h>
#include <wayland-server.h>
#include <clutter/clutter.h>

typedef struct _TWSCompositor TWSCompositor;

typedef struct
{
  struct wl_buffer *wayland_buffer;
  GList *surfaces_attached_to;
  struct wl_listener buffer_destroy_listener;
} TWSBuffer;

typedef struct
{
  struct wl_surface wayland_surface;
  TWSCompositor *compositor;
  int x;
  int y;
  TWSBuffer *buffer;
  ClutterActor *actor;
  gboolean has_shell_surface;
  struct wl_listener surface_destroy_listener;
} TWSSurface;

#endif /* __TWS_COMPOSITOR_H__ */
