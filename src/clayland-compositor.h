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

#ifndef __CLAYLAND_COMPOSITOR_H__
#define __CLAYLAND_COMPOSITOR_H__

#include <glib.h>
#include <wayland-server.h>
#include <clutter/clutter.h>
#include <cairo.h>

typedef struct _ClaylandCompositor ClaylandCompositor;

typedef struct
{
  struct wl_surface wayland_surface;
  ClaylandCompositor *compositor;
  int x;
  int y;
  struct wl_buffer *buffer;
  struct wl_listener buffer_destroy_listener;
  ClutterActor *actor;
  gboolean has_shell_surface;

  /* All the pending state, that wl_surface.commit will apply. */
  struct
  {
    /* wl_surface.attach */
    struct wl_buffer *buffer;
    struct wl_listener buffer_destroy_listener;
    int32_t sx;
    int32_t sy;

    /* wl_surface.damage */
    cairo_region_t *damage;

    /* wl_surface.frame */
    struct wl_list frame_callback_list;
  } pending;
} ClaylandSurface;

#endif /* __CLAYLAND_COMPOSITOR_H__ */
