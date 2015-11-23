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

#include "backends/meta-remote-desktop.h"

#include <gst/gst.h>
#include <meta/errors.h>

#include "meta-dbus-remote-desktop.h"

#define META_REMOTE_DESKTOP_DBUS_SERVICE "org.gnome.Mutter.RemoteDesktop"
#define META_REMOTE_DESKTOP_DBUS_PATH "/org/gnome/Mutter/RemoteDesktop"

struct _MetaRemoteDesktop
{
  MetaDBusRemoteDesktopSkeleton parent;

  GstElement *pipeline;

  int dbus_name_id;
};

static void meta_remote_desktop_init_iface (MetaDBusRemoteDesktopIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaRemoteDesktop,
                         meta_remote_desktop,
                         META_DBUS_TYPE_REMOTE_DESKTOP_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_REMOTE_DESKTOP,
                                                meta_remote_desktop_init_iface));

static void
meta_remote_desktop_pipeline_closed (MetaRemoteDesktop *rd)
{
  gst_element_set_state (rd->pipeline, GST_STATE_NULL);
  rd->pipeline = NULL;
}

static gboolean
meta_remote_desktop_pipeline_bus_watch (GstBus     *bus,
                                                GstMessage *message,
                                                gpointer    data)
{
  MetaRemoteDesktop *rd = data;
  GError *error = NULL;

  switch (message->type)
    {
    case GST_MESSAGE_EOS:
      meta_remote_desktop_pipeline_closed (rd);

      g_print ("Bus closed\n");

      return FALSE;

    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      g_warning ("Error in remote desktop video pipeline: %s\n",
                 error->message);
      g_error_free (error);

      meta_remote_desktop_pipeline_closed (rd);

      g_print ("Bus closed\n");

      return FALSE;

    default:
      break;
    }

  return TRUE;
}

static gboolean
meta_remote_desktop_open_pipeline (MetaRemoteDesktop *rd)
{
  GstElement *pipeline;
  GstBus *bus;
  GError *error = NULL;

  const gchar pipeline_descr[] =
    "videotestsrc ! pinossink";

  pipeline = gst_parse_launch_full (pipeline_descr,
                                    NULL,
                                    GST_PARSE_FLAG_FATAL_ERRORS,
                                    &error);
  if (!pipeline)
    {
      g_warning ("Couldn't start remote desktop pinos sink: %s",
                 error->message);
      return FALSE;
    }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus,
                     meta_remote_desktop_pipeline_bus_watch,
                     rd);
  gst_object_unref (bus);

  rd->pipeline = pipeline;

  return TRUE;
}

static void
meta_remote_desktop_close_pipeline (MetaRemoteDesktop *rd)
{
  gst_element_send_event (rd->pipeline, gst_event_new_eos ());

}

static gboolean
meta_remote_desktop_handle_start (MetaDBusRemoteDesktop *skeleton,
                                  GDBusMethodInvocation *invocation)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (skeleton);

  fprintf (stderr, "RD: start\n");

  if (!meta_remote_desktop_open_pipeline (rd))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to initiate remote desktop");
      return TRUE;
    }

  meta_dbus_remote_desktop_complete_start (skeleton, invocation);

  return TRUE;
}

static gboolean
meta_remote_desktop_handle_stop (MetaDBusRemoteDesktop *skeleton,
                                 GDBusMethodInvocation *invocation)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (skeleton);

  fprintf (stderr, "RD: stop\n");

  meta_remote_desktop_close_pipeline (rd);

  meta_dbus_remote_desktop_complete_stop (skeleton, invocation);

  return TRUE;
}

static void
meta_remote_desktop_init_iface (MetaDBusRemoteDesktopIface *iface)
{
  iface->handle_start = meta_remote_desktop_handle_start;
  iface->handle_stop = meta_remote_desktop_handle_stop;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  MetaRemoteDesktop *rd = user_data;
  GError *error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (rd),
                                         connection,
                                         META_REMOTE_DESKTOP_DBUS_PATH,
                                         &error))
    meta_warning ("Failed to export remote desktop object: %s\n", error->message);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Acquired name %s\n", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Lost or failed to acquire name %s\n", name);
}

static void
initialize_dbus_interface (MetaRemoteDesktop *rd)
{
  rd->dbus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                     META_REMOTE_DESKTOP_DBUS_SERVICE,
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     on_bus_acquired,
                                     on_name_acquired,
                                     on_name_lost,
                                     g_object_ref (rd),
                                     g_object_unref);
}

static void
meta_remote_desktop_init (MetaRemoteDesktop *rd)
{
  gst_init (NULL, NULL);
}

static void
meta_remote_desktop_constructed (GObject *object)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (object);

  initialize_dbus_interface (rd);
}

static void
meta_remote_desktop_finalize (GObject *object)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (object);

  if (rd->dbus_name_id != 0)
    {
      g_bus_unown_name (rd->dbus_name_id);
      rd->dbus_name_id = 0;
    }

  G_OBJECT_CLASS (meta_remote_desktop_parent_class)->finalize (object);
}

static void
meta_remote_desktop_class_init (MetaRemoteDesktopClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = meta_remote_desktop_constructed;
  object_class->finalize = meta_remote_desktop_finalize;
}
