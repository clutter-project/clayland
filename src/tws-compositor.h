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
