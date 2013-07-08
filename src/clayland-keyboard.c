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

/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <glib.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "clayland-keyboard.h"

static ClaylandSeat *
clayland_keyboard_get_seat (ClaylandKeyboard *keyboard)
{
  ClaylandSeat *seat = wl_container_of (keyboard, seat, keyboard);

  return seat;
}

static int
create_tmpfile_cloexec (char *tmpname)
{
  int fd;

#ifdef HAVE_MKOSTEMP
  fd = mkostemp (tmpname, O_CLOEXEC);
  if (fd >= 0)
    unlink (tmpname);
#else
  fd = mkstemp (tmpname);
  if (fd >= 0)
    {
      fd = set_cloexec_or_close (fd);
      unlink (tmpname);
    }
#endif

  return fd;
}

static int
create_anonymous_file (off_t size,
                       GError **error)
{
  static const char template[] = "weston-shared-XXXXXX";
  const char *path;
  char *name;
  int fd;

  path = g_getenv ("XDG_RUNTIME_DIR");
  if (!path)
    {
      errno = ENOENT;
      return -1;
    }

  name = g_build_filename (path, template, NULL);

  fd = create_tmpfile_cloexec (name);

  free (name);

  if (fd < 0)
    return -1;

  if (ftruncate (fd, size) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

static gboolean
clayland_xkb_info_new_keymap (ClaylandXkbInfo *xkb_info)
{
  GError *error = NULL;
  char *keymap_str;

  xkb_info->shift_mod =
    xkb_map_mod_get_index (xkb_info->keymap, XKB_MOD_NAME_SHIFT);
  xkb_info->caps_mod =
    xkb_map_mod_get_index (xkb_info->keymap, XKB_MOD_NAME_CAPS);
  xkb_info->ctrl_mod =
    xkb_map_mod_get_index (xkb_info->keymap, XKB_MOD_NAME_CTRL);
  xkb_info->alt_mod =
    xkb_map_mod_get_index (xkb_info->keymap, XKB_MOD_NAME_ALT);
  xkb_info->mod2_mod = xkb_map_mod_get_index (xkb_info->keymap, "Mod2");
  xkb_info->mod3_mod = xkb_map_mod_get_index (xkb_info->keymap, "Mod3");
  xkb_info->super_mod =
    xkb_map_mod_get_index (xkb_info->keymap, XKB_MOD_NAME_LOGO);
  xkb_info->mod5_mod = xkb_map_mod_get_index (xkb_info->keymap, "Mod5");

  keymap_str = xkb_map_get_as_string (xkb_info->keymap);
  if (keymap_str == NULL)
    {
      g_warning ("failed to get string version of keymap\n");
      return FALSE;
    }
  xkb_info->keymap_size = strlen (keymap_str) + 1;

  xkb_info->keymap_fd = create_anonymous_file (xkb_info->keymap_size, &error);
  if (xkb_info->keymap_fd < 0)
    {
      g_warning ("creating a keymap file for %lu bytes failed: %s\n",
                 (unsigned long) xkb_info->keymap_size,
                 error->message);
      g_clear_error (&error);
      goto err_keymap_str;
    }

  xkb_info->keymap_area = mmap (NULL, xkb_info->keymap_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, xkb_info->keymap_fd, 0);
  if (xkb_info->keymap_area == MAP_FAILED)
    {
      g_warning ("failed to mmap() %lu bytes\n",
                 (unsigned long) xkb_info->keymap_size);
      goto err_dev_zero;
    }
  strcpy (xkb_info->keymap_area, keymap_str);
  free (keymap_str);

  return TRUE;

err_dev_zero:
  close (xkb_info->keymap_fd);
  xkb_info->keymap_fd = -1;
err_keymap_str:
  free (keymap_str);
  return FALSE;
}

static gboolean
clayland_keyboard_build_global_keymap (struct xkb_context *xkb_context,
                                       struct xkb_rule_names *xkb_names,
                                       ClaylandXkbInfo *xkb_info)
{
  xkb_info->keymap = xkb_map_new_from_names (xkb_context,
                                             xkb_names,
                                             0 /* flags */);
  if (xkb_info->keymap == NULL)
    {
      g_warning ("failed to compile global XKB keymap\n"
                 "  tried rules %s, model %s, layout %s, variant %s, "
                 "options %s\n",
                 xkb_names->rules,
                 xkb_names->model,
                 xkb_names->layout,
                 xkb_names->variant,
                 xkb_names->options);
      return FALSE;
    }

  if (!clayland_xkb_info_new_keymap (xkb_info))
    return FALSE;

  return TRUE;
}

static void
lose_keyboard_focus (struct wl_listener *listener, void *data)
{
  ClaylandKeyboard *keyboard =
    wl_container_of (listener, keyboard, focus_listener);

  keyboard->focus_resource = NULL;
}

static void
default_grab_key (ClaylandKeyboardGrab *grab,
                  uint32_t time, uint32_t key, uint32_t state)
{
  ClaylandKeyboard *keyboard = grab->keyboard;
  struct wl_resource *resource;
  uint32_t serial;

  resource = keyboard->focus_resource;
  if (resource)
    {
      struct wl_client *client = wl_resource_get_client (resource);
      struct wl_display *display = wl_client_get_display (client);
      serial = wl_display_next_serial (display);
      wl_keyboard_send_key (resource, serial, time, key, state);
    }
}

static struct wl_resource *
find_resource_for_surface (struct wl_list *list, ClaylandSurface *surface)
{
  struct wl_client *client;

  if (!surface)
    return NULL;

  if (!surface->resource)
    return NULL;

  client = wl_resource_get_client (surface->resource);

  return wl_resource_find_for_client (list, client);
}

static void
default_grab_modifiers (ClaylandKeyboardGrab *grab, uint32_t serial,
                        uint32_t mods_depressed, uint32_t mods_latched,
                        uint32_t mods_locked, uint32_t group)
{
  ClaylandKeyboard *keyboard = grab->keyboard;
  ClaylandSeat *seat = clayland_keyboard_get_seat (keyboard);
  ClaylandPointer *pointer = &seat->pointer;
  struct wl_resource *resource, *pr;

  resource = keyboard->focus_resource;
  if (!resource)
    return;

  wl_keyboard_send_modifiers (resource, serial, mods_depressed,
                              mods_latched, mods_locked, group);

  if (pointer && pointer->focus && pointer->focus != keyboard->focus)
    {
      pr = find_resource_for_surface (&keyboard->resource_list,
                                      pointer->focus);
      if (pr)
        {
          wl_keyboard_send_modifiers (pr,
                                      serial,
                                      keyboard->modifiers.mods_depressed,
                                      keyboard->modifiers.mods_latched,
                                      keyboard->modifiers.mods_locked,
                                      keyboard->modifiers.group);
        }
    }
}

static const ClaylandKeyboardGrabInterface
  default_keyboard_grab_interface = {
  default_grab_key,
  default_grab_modifiers,
};

gboolean
clayland_keyboard_init (ClaylandKeyboard *keyboard,
                        struct wl_display *display)
{
  memset (keyboard, 0, sizeof *keyboard);

  wl_list_init (&keyboard->resource_list);
  wl_array_init (&keyboard->keys);
  keyboard->focus_listener.notify = lose_keyboard_focus;
  keyboard->default_grab.interface = &default_keyboard_grab_interface;
  keyboard->default_grab.keyboard = keyboard;
  keyboard->grab = &keyboard->default_grab;
  wl_signal_init (&keyboard->focus_signal);

  keyboard->display = display;

  keyboard->xkb_context = xkb_context_new (0 /* flags */);

  clayland_keyboard_build_global_keymap (keyboard->xkb_context,
                                         &keyboard->xkb_names,
                                         &keyboard->xkb_info);

  return TRUE;
}

static void
clayland_xkb_info_destroy (ClaylandXkbInfo *xkb_info)
{
  if (xkb_info->keymap)
    xkb_map_unref (xkb_info->keymap);

  if (xkb_info->keymap_area)
    munmap (xkb_info->keymap_area, xkb_info->keymap_size);
  if (xkb_info->keymap_fd >= 0)
    close (xkb_info->keymap_fd);
}

static void
set_modifiers (ClaylandKeyboard *keyboard,
               guint32 serial,
               ClutterModifierType modifier_state)
{
  ClaylandKeyboardGrab *grab = keyboard->grab;
  uint32_t depressed_mods = 0;
  uint32_t locked_mods = 0;

  if (keyboard->last_modifier_state == modifier_state)
    return;

  if ((modifier_state & CLUTTER_SHIFT_MASK) &&
      keyboard->xkb_info.shift_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.shift_mod);

  if ((modifier_state & CLUTTER_LOCK_MASK) &&
      keyboard->xkb_info.caps_mod != XKB_MOD_INVALID)
    locked_mods |= (1 << keyboard->xkb_info.caps_mod);

  if ((modifier_state & CLUTTER_CONTROL_MASK) &&
      keyboard->xkb_info.ctrl_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.ctrl_mod);

  if ((modifier_state & CLUTTER_MOD1_MASK) &&
      keyboard->xkb_info.alt_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.alt_mod);

  if ((modifier_state & CLUTTER_MOD2_MASK) &&
      keyboard->xkb_info.mod2_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.mod2_mod);

  if ((modifier_state & CLUTTER_MOD3_MASK) &&
      keyboard->xkb_info.mod3_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.mod3_mod);

  if ((modifier_state & CLUTTER_SUPER_MASK) &&
      keyboard->xkb_info.super_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.super_mod);

  if ((modifier_state & CLUTTER_MOD5_MASK) &&
      keyboard->xkb_info.mod5_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << keyboard->xkb_info.mod5_mod);

  keyboard->last_modifier_state = modifier_state;

  grab->interface->modifiers (grab,
                              serial,
                              depressed_mods,
                              0, /* latched_modes */
                              locked_mods,
                              0 /* group */);
}

void
clayland_keyboard_handle_event (ClaylandKeyboard *keyboard,
                                const ClutterKeyEvent *event)
{
  gboolean state = event->type == CLUTTER_KEY_PRESS;
  guint evdev_code;
  uint32_t serial;

  /* We can't do anything with the event if we can't get an evdev
     keycode for it */
  if (event->device == NULL ||
      !clutter_input_device_keycode_to_evdev (event->device,
                                              event->hardware_keycode,
                                              &evdev_code))
    return;

  /* We want to ignore events that are sent because of auto-repeat. In
     the Clutter event stream these appear as a single key press
     event. We can detect that because the key will already have been
     pressed */
  if (state)
    {
      uint32_t *end = (void *) ((char *) keyboard->keys.data +
                                keyboard->keys.size);
      uint32_t *k;

      /* Ignore the event if the key is already down */
      for (k = keyboard->keys.data; k < end; k++)
        if (*k == evdev_code)
          return;

      /* Otherwise add the key to the list of pressed keys */
      k = wl_array_add (&keyboard->keys, sizeof (*k));
      *k = evdev_code;
    }
  else
    {
      uint32_t *end = (void *) ((char *) keyboard->keys.data +
                                keyboard->keys.size);
      uint32_t *k;

      /* Remove the key from the array */
      for (k = keyboard->keys.data; k < end; k++)
        if (*k == evdev_code)
          {
            *k = *(end - 1);
            keyboard->keys.size -= sizeof (*k);

            goto found;
          }

      g_warning ("unexpected key release event for key 0x%x", evdev_code);

    found:
      (void) 0;
    }

  serial = wl_display_next_serial (keyboard->display);

  set_modifiers (keyboard, serial, event->modifier_state);

  keyboard->grab->interface->key (keyboard->grab,
                                  event->time,
                                  evdev_code,
                                  state);
}

void
clayland_keyboard_set_focus (ClaylandKeyboard *keyboard,
                             ClaylandSurface *surface)
{
  struct wl_resource *resource;
  uint32_t serial;

  if (keyboard->focus_resource && keyboard->focus != surface)
    {
      struct wl_display *display;
      struct wl_client *client;

      resource = keyboard->focus_resource;
      client = wl_resource_get_client (resource);
      display = wl_client_get_display (client);
      serial = wl_display_next_serial (display);
      wl_keyboard_send_leave (resource, serial, keyboard->focus->resource);
      wl_list_remove (&keyboard->focus_listener.link);
    }

  resource = find_resource_for_surface (&keyboard->resource_list, surface);
  if (resource &&
      (keyboard->focus != surface || keyboard->focus_resource != resource))
    {
      struct wl_display *display;
      struct wl_client *client = wl_resource_get_client (resource);

      display = wl_client_get_display (client);
      serial = wl_display_next_serial (display);
      wl_keyboard_send_modifiers (resource, serial,
                                  keyboard->modifiers.mods_depressed,
                                  keyboard->modifiers.mods_latched,
                                  keyboard->modifiers.mods_locked,
                                  keyboard->modifiers.group);
      wl_keyboard_send_enter (resource,
                              serial,
                              surface->resource,
                              &keyboard->keys);
      wl_resource_add_destroy_listener (resource, &keyboard->focus_listener);
      keyboard->focus_serial = serial;
    }

  keyboard->focus_resource = resource;
  keyboard->focus = surface;
  wl_signal_emit (&keyboard->focus_signal, keyboard);
}

void
clayland_keyboard_start_grab (ClaylandKeyboard *keyboard,
                              ClaylandKeyboardGrab *grab)
{
  keyboard->grab = grab;
  grab->keyboard = keyboard;

  /* XXX focus? */
}

void
clayland_keyboard_end_grab (ClaylandKeyboard *keyboard)
{
  keyboard->grab = &keyboard->default_grab;
}

void
clayland_keyboard_release (ClaylandKeyboard *keyboard)
{
  g_free ((char *) keyboard->xkb_names.rules);
  g_free ((char *) keyboard->xkb_names.model);
  g_free ((char *) keyboard->xkb_names.layout);
  g_free ((char *) keyboard->xkb_names.variant);
  g_free ((char *) keyboard->xkb_names.options);

  clayland_xkb_info_destroy (&keyboard->xkb_info);
  xkb_context_unref (keyboard->xkb_context);

  /* XXX: What about keyboard->resource_list? */
  if (keyboard->focus_resource)
    wl_list_remove (&keyboard->focus_listener.link);
  wl_array_release (&keyboard->keys);
}
