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

#include <gst/gst.h>

struct _MetaRemoteDesktopSession
{
  GObject parent;

  MetaRemoteDesktop *rd;

  GstElement *pipeline;
};

G_DEFINE_TYPE (MetaRemoteDesktopSession,
               meta_remote_desktop_session,
               G_TYPE_OBJECT);

static void
meta_remote_desktop_session_pipeline_closed (MetaRemoteDesktopSession *session)
{
  gst_element_set_state (session->pipeline, GST_STATE_NULL);
  session->pipeline = NULL;

  g_object_unref (session);
}

static gboolean
meta_remote_desktop_session_pipeline_bus_watch (GstBus     *bus,
                                                GstMessage *message,
                                                gpointer    data)
{
  MetaRemoteDesktopSession *session = data;
  GError *error = NULL;

  switch (message->type)
    {
    case GST_MESSAGE_EOS:
      meta_remote_desktop_session_pipeline_closed (session);

      g_print ("Bus closed\n");

      return FALSE;

    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      g_warning ("Error in remote desktop video pipeline: %s\n",
                 error->message);
      g_error_free (error);

      meta_remote_desktop_session_pipeline_closed (session);

      g_print ("Bus closed\n");

      return FALSE;

    default:
      break;
    }

  return TRUE;
}

static gboolean
meta_remote_desktop_session_open_pipeline (MetaRemoteDesktopSession *session)
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
                     meta_remote_desktop_session_pipeline_bus_watch,
                     session);
  gst_object_unref (bus);

  session->pipeline = pipeline;

  g_object_ref (session);

  return TRUE;
}

static void
meta_remote_desktop_session_close_pipeline (MetaRemoteDesktopSession *session)
{
  gst_element_send_event (session->pipeline, gst_event_new_eos ());
}

gboolean
meta_remote_desktop_session_start (MetaRemoteDesktopSession *session)
{
  if (!meta_remote_desktop_session_open_pipeline (session))
    return FALSE;

  return TRUE;
}

void
meta_remote_desktop_session_stop (MetaRemoteDesktopSession *session)
{
  meta_remote_desktop_session_close_pipeline (session);
}

gboolean
meta_remote_desktop_session_is_running (MetaRemoteDesktopSession *session)
{
  return session->pipeline != NULL;
}

MetaRemoteDesktopSession *
meta_remote_desktop_session_new (MetaRemoteDesktop *rd)
{
  MetaRemoteDesktopSession *session;

  session = g_object_new (META_TYPE_REMOTE_DESKTOP_SESSION, NULL);
  session->rd = rd;

  return session;
}

static void
meta_remote_desktop_session_init (MetaRemoteDesktopSession *session)
{
}

static void
meta_remote_desktop_session_finalize (GObject *object)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (object);

  if (meta_remote_desktop_session_is_running (session))
    meta_remote_desktop_session_stop (session);

  G_OBJECT_CLASS (meta_remote_desktop_session_parent_class)->finalize (object);
}

static void
meta_remote_desktop_session_class_init (MetaRemoteDesktopSessionClass *klass)
{

  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_remote_desktop_session_finalize;
}
