/*
 * test-wayland-surface
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

#include "config.h"

#include <glib.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "tws-keyboard.h"

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
tws_xkb_info_new_keymap (TwsXkbInfo *xkb_info)
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
tws_keyboard_build_global_keymap (struct xkb_context *xkb_context,
                                  struct xkb_rule_names *xkb_names,
                                  TwsXkbInfo *xkb_info)
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

  if (!tws_xkb_info_new_keymap (xkb_info))
    return FALSE;

  return TRUE;
}

gboolean
tws_keyboard_init (TwsKeyboard *keyboard,
                   struct wl_display *display)
{
  wl_keyboard_init (&keyboard->parent);

  keyboard->display = display;

  memset (&keyboard->xkb_names, 0, sizeof (keyboard->xkb_names));

  keyboard->xkb_context = xkb_context_new (0 /* flags */);

  tws_keyboard_build_global_keymap (keyboard->xkb_context,
                                    &keyboard->xkb_names,
                                    &keyboard->xkb_info);

  return TRUE;
}

static void
tws_xkb_info_destroy (TwsXkbInfo *xkb_info)
{
  if (xkb_info->keymap)
    xkb_map_unref (xkb_info->keymap);

  if (xkb_info->keymap_area)
    munmap (xkb_info->keymap_area, xkb_info->keymap_size);
  if (xkb_info->keymap_fd >= 0)
    close (xkb_info->keymap_fd);
}

static void
set_modifiers (TwsKeyboard *tws_keyboard,
               guint32 serial,
               ClutterModifierType modifier_state)
{
  struct wl_keyboard *keyboard = &tws_keyboard->parent;
  struct wl_keyboard_grab *grab = keyboard->grab;
  uint32_t depressed_mods = 0;
  uint32_t locked_mods = 0;

  if (tws_keyboard->last_modifier_state == modifier_state)
    return;

  if ((modifier_state & CLUTTER_SHIFT_MASK) &&
      tws_keyboard->xkb_info.shift_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.shift_mod);

  if ((modifier_state & CLUTTER_LOCK_MASK) &&
      tws_keyboard->xkb_info.caps_mod != XKB_MOD_INVALID)
    locked_mods |= (1 << tws_keyboard->xkb_info.caps_mod);

  if ((modifier_state & CLUTTER_CONTROL_MASK) &&
      tws_keyboard->xkb_info.ctrl_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.ctrl_mod);

  if ((modifier_state & CLUTTER_MOD1_MASK) &&
      tws_keyboard->xkb_info.alt_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.alt_mod);

  if ((modifier_state & CLUTTER_MOD2_MASK) &&
      tws_keyboard->xkb_info.mod2_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.mod2_mod);

  if ((modifier_state & CLUTTER_MOD3_MASK) &&
      tws_keyboard->xkb_info.mod3_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.mod3_mod);

  if ((modifier_state & CLUTTER_SUPER_MASK) &&
      tws_keyboard->xkb_info.super_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.super_mod);

  if ((modifier_state & CLUTTER_MOD5_MASK) &&
      tws_keyboard->xkb_info.mod5_mod != XKB_MOD_INVALID)
    depressed_mods |= (1 << tws_keyboard->xkb_info.mod5_mod);

  tws_keyboard->last_modifier_state = modifier_state;

  grab->interface->modifiers (grab,
                              serial,
                              depressed_mods,
                              0, /* latched_modes */
                              locked_mods,
                              0 /* group */);
}

void
tws_keyboard_handle_event (TwsKeyboard *tws_keyboard,
                           const ClutterKeyEvent *event)
{
  struct wl_keyboard *keyboard = &tws_keyboard->parent;
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

  serial = wl_display_next_serial (tws_keyboard->display);

  set_modifiers (tws_keyboard, serial, event->modifier_state);

  keyboard->grab->interface->key (keyboard->grab,
                                  event->time,
                                  evdev_code,
                                  state);
}

void
tws_keyboard_release (TwsKeyboard *keyboard)
{
  g_free ((char *) keyboard->xkb_names.rules);
  g_free ((char *) keyboard->xkb_names.model);
  g_free ((char *) keyboard->xkb_names.layout);
  g_free ((char *) keyboard->xkb_names.variant);
  g_free ((char *) keyboard->xkb_names.options);

  tws_xkb_info_destroy (&keyboard->xkb_info);
  xkb_context_unref (keyboard->xkb_context);

  wl_keyboard_release (&keyboard->parent);
}
