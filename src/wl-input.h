/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef __WL_INPUT_H__
#define __WL_INPUT_H__

#include <wayland-server.h>

struct cwl_seat;
struct cwl_pointer;
struct cwl_keyboard;

struct cwl_pointer_grab;
struct cwl_pointer_grab_interface
{
  void (*focus) (struct cwl_pointer_grab * grab,
                 struct wl_surface * surface, wl_fixed_t x, wl_fixed_t y);
  void (*motion) (struct cwl_pointer_grab * grab,
                  uint32_t time, wl_fixed_t x, wl_fixed_t y);
  void (*button) (struct cwl_pointer_grab * grab,
                  uint32_t time, uint32_t button, uint32_t state);
};

struct cwl_pointer_grab
{
  const struct cwl_pointer_grab_interface *interface;
  struct cwl_pointer *pointer;
  struct wl_surface *focus;
  wl_fixed_t x, y;
};

struct cwl_keyboard_grab;
struct cwl_keyboard_grab_interface
{
  void (*key) (struct cwl_keyboard_grab * grab, uint32_t time,
               uint32_t key, uint32_t state);
  void (*modifiers) (struct cwl_keyboard_grab * grab, uint32_t serial,
                     uint32_t mods_depressed, uint32_t mods_latched,
                     uint32_t mods_locked, uint32_t group);
};

struct cwl_keyboard_grab
{
  const struct cwl_keyboard_grab_interface *interface;
  struct cwl_keyboard *keyboard;
  struct wl_surface *focus;
  uint32_t key;
};

struct cwl_data_offer
{
  struct wl_resource resource;
  struct cwl_data_source *source;
  struct wl_listener source_destroy_listener;
};

struct cwl_data_source
{
  struct wl_resource resource;
  struct wl_array mime_types;

  void (*accept) (struct cwl_data_source * source,
                  uint32_t serial, const char *mime_type);
  void (*send) (struct cwl_data_source * source,
                const char *mime_type, int32_t fd);
  void (*cancel) (struct cwl_data_source * source);
};

struct cwl_pointer
{
  struct cwl_seat *seat;

  struct wl_list resource_list;
  struct wl_surface *focus;
  struct wl_resource *focus_resource;
  struct wl_listener focus_listener;
  uint32_t focus_serial;
  struct wl_signal focus_signal;

  struct cwl_pointer_grab *grab;
  struct cwl_pointer_grab default_grab;
  wl_fixed_t grab_x, grab_y;
  uint32_t grab_button;
  uint32_t grab_serial;
  uint32_t grab_time;

  wl_fixed_t x, y;
  struct wl_surface *current;
  struct wl_listener current_listener;
  wl_fixed_t current_x, current_y;

  uint32_t button_count;
};

struct cwl_keyboard
{
  struct cwl_seat *seat;

  struct wl_list resource_list;
  struct wl_surface *focus;
  struct wl_resource *focus_resource;
  struct wl_listener focus_listener;
  uint32_t focus_serial;
  struct wl_signal focus_signal;

  struct cwl_keyboard_grab *grab;
  struct cwl_keyboard_grab default_grab;
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
};

struct cwl_seat
{
  struct wl_list base_resource_list;
  struct wl_signal destroy_signal;

  struct cwl_pointer *pointer;
  struct cwl_keyboard *keyboard;

  uint32_t selection_serial;
  struct cwl_data_source *selection_data_source;
  struct wl_listener selection_data_source_listener;
  struct wl_signal selection_signal;

  struct wl_list drag_resource_list;
  struct wl_client *drag_client;
  struct cwl_data_source *drag_data_source;
  struct wl_listener drag_data_source_listener;
  struct wl_surface *drag_focus;
  struct wl_resource *drag_focus_resource;
  struct wl_listener drag_focus_listener;
  struct cwl_pointer_grab drag_grab;
  struct wl_surface *drag_surface;
  struct wl_listener drag_icon_listener;
  struct wl_signal drag_icon_signal;
};

void cwl_seat_init (struct cwl_seat *seat);

void cwl_seat_release (struct cwl_seat *seat);

void cwl_seat_set_pointer (struct cwl_seat *seat, struct cwl_pointer *pointer);
void
cwl_seat_set_keyboard (struct cwl_seat *seat, struct cwl_keyboard *keyboard);

void cwl_pointer_init (struct cwl_pointer *pointer);
void cwl_pointer_release (struct cwl_pointer *pointer);
void
cwl_pointer_set_focus (struct cwl_pointer *pointer, struct wl_surface *surface,
                       wl_fixed_t sx, wl_fixed_t sy);
void
cwl_pointer_start_grab (struct cwl_pointer *pointer,
                        struct cwl_pointer_grab *grab);
void cwl_pointer_end_grab (struct cwl_pointer *pointer);
void
cwl_pointer_set_current (struct cwl_pointer *pointer,
                         struct wl_surface *surface);

void cwl_keyboard_init (struct cwl_keyboard *keyboard);
void cwl_keyboard_release (struct cwl_keyboard *keyboard);
void
cwl_keyboard_set_focus (struct cwl_keyboard *keyboard,
                        struct wl_surface *surface);
void cwl_keyboard_start_grab (struct cwl_keyboard *device,
                              struct cwl_keyboard_grab *grab);
void cwl_keyboard_end_grab (struct cwl_keyboard *keyboard);

void cwl_data_device_set_keyboard_focus (struct cwl_seat *seat);

int cwl_data_device_manager_init (struct wl_display *display);


void
cwl_seat_set_selection (struct cwl_seat *seat,
                        struct cwl_data_source *source, uint32_t serial);

#endif /* __WL_SEAT_H__ */
