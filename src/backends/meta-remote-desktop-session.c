/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2015 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include "backends/meta-remote-desktop-session.h"

#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

#include "backends/native/meta-backend-native.h"
#include "backends/x11/meta-backend-x11.h"
#include "backends/meta-remote-desktop-src.h"
#include "cogl/cogl.h"
#include "meta/meta-backend.h"
#include "meta/errors.h"
#include "meta-dbus-remote-desktop.h"

#define META_REMOTE_DESKTOP_SESSION_DBUS_PATH "/org/gnome/Mutter/RemoteDesktop/Session"

enum
{
  STOPPED,

  LAST_SIGNAL
};

guint signals[LAST_SIGNAL];

struct _MetaRemoteDesktopSession
{
  MetaDBusRemoteDesktopSessionSkeleton parent;

  MetaRemoteDesktop *rd;

  MetaRemoteDesktopSrc *src;
  char *stream_id;

  char *object_path;

  ClutterActor *stage;
  int width;
  int height;

  ClutterVirtualInputDevice *virtual_pointer;
  ClutterVirtualInputDevice *virtual_keyboard;
};

static void
meta_remote_desktop_session_init_iface (MetaDBusRemoteDesktopSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaRemoteDesktopSession,
                         meta_remote_desktop_session,
                         META_DBUS_TYPE_REMOTE_DESKTOP_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_REMOTE_DESKTOP_SESSION,
                                                meta_remote_desktop_session_init_iface));

static gboolean
meta_remote_desktop_session_init_stream (MetaRemoteDesktopSession *session)
{
  MetaDBusRemoteDesktopSession *skeleton =
    META_DBUS_REMOTE_DESKTOP_SESSION  (session);
  char *stream_id;
  MetaRemoteDesktopSrc *src;
  static unsigned int global_stream_id = 0;

  stream_id = g_strdup_printf ("%u", ++global_stream_id);
  src = meta_remote_desktop_src_new (stream_id,
                                     &(MetaRectangle) {
                                       .width = session->width,
                                       .height = session->height
                                     });
  session->src = src;
  session->stream_id = stream_id;

  meta_dbus_remote_desktop_session_set_pinos_stream_id (skeleton, stream_id);

  return TRUE;
}

static void
meta_remote_desktop_session_on_stage_paint (ClutterActor             *actor,
                                            MetaRemoteDesktopSession *session)
{
  meta_remote_desktop_src_maybe_record_frame (session->src,
                                              CLUTTER_STAGE (session->stage));
}

gboolean
meta_remote_desktop_session_start (MetaRemoteDesktopSession *session)
{
  if (!meta_remote_desktop_session_init_stream (session))
    return FALSE;

  g_signal_connect_after (session->stage, "paint",
                          G_CALLBACK (meta_remote_desktop_session_on_stage_paint),
                          session);
  clutter_actor_queue_redraw (CLUTTER_ACTOR (session->stage));

  return TRUE;
}

void
meta_remote_desktop_session_stop (MetaRemoteDesktopSession *session)
{
  MetaDBusRemoteDesktopSession *skeleton =
    META_DBUS_REMOTE_DESKTOP_SESSION  (session);
  g_clear_object (&session->src);
  g_clear_pointer (&session->stream_id, g_free);

  meta_dbus_remote_desktop_session_set_pinos_stream_id (skeleton, NULL);

  g_signal_handlers_disconnect_by_func (session->stage,
                                        (gpointer) meta_remote_desktop_session_on_stage_paint,
                                        session);

  g_signal_emit (session, signals[STOPPED], 0);
}

gboolean
meta_remote_desktop_session_is_running (MetaRemoteDesktopSession *session)
{
  return !!session->src;
}

const char *
meta_remote_desktop_session_get_object_path (MetaRemoteDesktopSession *session)
{
  return session->object_path;
}

MetaRemoteDesktopSession *
meta_remote_desktop_session_new (MetaRemoteDesktop *rd)
{
  MetaRemoteDesktopSession *session;
  GDBusConnection *connection;
  GError *error = NULL;
  static unsigned int global_session_number = 0;

  session = g_object_new (META_TYPE_REMOTE_DESKTOP_SESSION, NULL);
  session->rd = rd;
  session->object_path =
    g_strdup_printf (META_REMOTE_DESKTOP_SESSION_DBUS_PATH "/u%u",
                     ++global_session_number);

  connection =
    g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (rd));
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         connection,
                                         session->object_path,
                                         &error))
    {
      meta_warning ("Failed to export session object: %s\n", error->message);
      return NULL;
    }

  return session;
}

static gboolean
handle_stop (MetaDBusRemoteDesktopSession *skeleton,
             GDBusMethodInvocation        *invocation)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);

  fprintf (stderr, "RD: stop\n");

  if (meta_remote_desktop_session_is_running (session))
    meta_remote_desktop_session_stop (session);

  meta_dbus_remote_desktop_session_complete_stop (skeleton, invocation);

  return TRUE;
}

static gboolean
handle_notify_keyboard_keysym (MetaDBusRemoteDesktopSession *skeleton,
                               GDBusMethodInvocation        *invocation,
                               guint                         keysym,
                               gboolean                      pressed)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);

  ClutterKeyState state;

  if (pressed)
    state = CLUTTER_KEY_STATE_PRESSED;
  else
    state = CLUTTER_KEY_STATE_RELEASED;

  clutter_virtual_input_device_notify_keyval (session->virtual_keyboard,
                                              CLUTTER_CURRENT_TIME,
                                              keysym,
                                              state);

  meta_dbus_remote_desktop_session_complete_notify_keyboard_keysym (skeleton,
                                                                    invocation);
  return TRUE;
}

/* Translation taken from the clutter evdev backend. */
static gint
translate_to_clutter_button (gint button)
{
  switch (button)
    {
    case BTN_LEFT:
      return CLUTTER_BUTTON_PRIMARY;
    case BTN_RIGHT:
      return CLUTTER_BUTTON_SECONDARY;
    case BTN_MIDDLE:
      return CLUTTER_BUTTON_MIDDLE;
    default:
      /* For compatibility reasons, all additional buttons go after the old 4-7
       * scroll ones.
       */
      return button - (BTN_LEFT - 1) + 4;
    }
}

static gboolean
handle_notify_pointer_button (MetaDBusRemoteDesktopSession *skeleton,
                              GDBusMethodInvocation        *invocation,
                              gint                          button_code,
                              gboolean                      pressed)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);
  uint32_t button;
  ClutterButtonState state;

  button = translate_to_clutter_button (button_code);

  if (pressed)
    state = CLUTTER_BUTTON_STATE_PRESSED;
  else
    state = CLUTTER_BUTTON_STATE_RELEASED;

  clutter_virtual_input_device_notify_button (session->virtual_pointer,
                                              CLUTTER_CURRENT_TIME,
                                              button,
                                              state);

  meta_dbus_remote_desktop_session_complete_notify_pointer_button (skeleton,
                                                                   invocation);

  return TRUE;
}

static ClutterScrollDirection
discrete_steps_to_scroll_direction (guint axis,
                                    gint  steps)
{
  if (axis == 0 && steps < 0)
    return CLUTTER_SCROLL_UP;
  if (axis == 0 && steps > 0)
    return CLUTTER_SCROLL_DOWN;
  if (axis == 1 && steps < 0)
    return CLUTTER_SCROLL_LEFT;
  if (axis == 1 && steps > 0)
    return CLUTTER_SCROLL_RIGHT;

  g_assert_not_reached ();
}

static gboolean
handle_notify_pointer_axis_discrete (MetaDBusRemoteDesktopSession *skeleton,
                                     GDBusMethodInvocation        *invocation,
                                     guint                         axis,
                                     gint                          steps)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);
  ClutterScrollDirection direction;

  if (axis <= 1)
    {
      meta_warning ("MetaRemoteDesktop: Invalid pointer axis\n");
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid axis value");
      return TRUE;
    }

  if (steps == 0)
    {
      meta_warning ("MetaRemoteDesktop: Invalid axis steps value\n");
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid axis steps value");
      return TRUE;
    }

  if (steps != -1 || steps != 1)
    meta_warning ("Multiple steps at the same time not yet implemented, treating as one.\n");

  /*
   * We don't have the actual scroll source, but only know they should be
   * considered as discrete steps. The device that produces such scroll events
   * is the scroll wheel, so pretend that is the scroll source.
   */
  direction = discrete_steps_to_scroll_direction (axis, steps);
  clutter_virtual_input_device_notify_discrete_scroll (session->virtual_pointer,
                                                       CLUTTER_CURRENT_TIME,
                                                       direction,
                                                       CLUTTER_SCROLL_SOURCE_WHEEL);

  meta_dbus_remote_desktop_session_complete_notify_pointer_axis_discrete (skeleton,
                                                                          invocation);

  return TRUE;
}

static gboolean
handle_notify_pointer_motion_absolute (MetaDBusRemoteDesktopSession *skeleton,
                                       GDBusMethodInvocation        *invocation,
                                       gdouble                       x,
                                       gdouble                       y)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);

  clutter_virtual_input_device_notify_absolute_motion (session->virtual_pointer,
                                                       CLUTTER_CURRENT_TIME,
                                                       x, y);

  meta_dbus_remote_desktop_session_complete_notify_pointer_motion_absolute (skeleton,
                                                                            invocation);

  return TRUE;
}

static void
meta_remote_desktop_session_init_iface (MetaDBusRemoteDesktopSessionIface *iface)
{
  iface->handle_stop = handle_stop;
  iface->handle_notify_keyboard_keysym = handle_notify_keyboard_keysym;
  iface->handle_notify_pointer_button = handle_notify_pointer_button;
  iface->handle_notify_pointer_axis_discrete = handle_notify_pointer_axis_discrete;
  iface->handle_notify_pointer_motion_absolute = handle_notify_pointer_motion_absolute;
}

static void
meta_remote_desktop_session_init (MetaRemoteDesktopSession *session)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterDeviceManager *device_manager =
    clutter_device_manager_get_default ();
  ClutterActor *stage = meta_backend_get_stage (backend);
  ClutterActorBox allocation;

  session->stage = stage;
  clutter_actor_get_allocation_box (session->stage, &allocation);
  session->width = (int)(0.5 + allocation.x2 - allocation.x1);
  session->height = (int)(0.5 + allocation.y2 - allocation.y1);

  session->virtual_pointer =
    clutter_device_manager_create_virtual_device (device_manager,
                                                  CLUTTER_POINTER_DEVICE);
  session->virtual_keyboard =
    clutter_device_manager_create_virtual_device (device_manager,
                                                  CLUTTER_KEYBOARD_DEVICE);
}

static void
meta_remote_desktop_session_finalize (GObject *object)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (object);

  g_assert (!meta_remote_desktop_session_is_running (session));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));

  g_free (session->stream_id);
  g_free (session->object_path);

  G_OBJECT_CLASS (meta_remote_desktop_session_parent_class)->finalize (object);
}

static void
meta_remote_desktop_session_class_init (MetaRemoteDesktopSessionClass *klass)
{

  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_remote_desktop_session_finalize;

  signals[STOPPED] = g_signal_new ("stopped",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
