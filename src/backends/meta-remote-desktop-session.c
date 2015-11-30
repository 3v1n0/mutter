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

#include <cogl/cogl.h>
#include <gst/gst.h>
#include <meta/meta-backend.h>
#include <meta/errors.h>

#include "backends/meta-remote-desktop-src.h"

#define DEFAULT_FRAMERATE 30

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstPad, gst_object_unref);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstElement, gst_object_unref);

struct _MetaRemoteDesktopSession
{
  GObject parent;

  MetaRemoteDesktop *rd;

  GstElement *pipeline;
  GstElement *src;

  ClutterActor *stage;
  int width;
  int height;

  GstClockTime last_frame_time;
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
meta_remote_desktop_session_add_source (MetaRemoteDesktopSession *session,
                                        GstElement               *pipeline)
{
  g_autoptr(GstPad) sink_pad = NULL;
  g_autoptr(GstPad) src_pad = NULL;
  MetaRemoteDesktopSrc *src;

  sink_pad = gst_bin_find_unlinked_pad (GST_BIN (pipeline), GST_PAD_SINK);
  if (sink_pad == NULL)
    {
      meta_warning ("MetaRemoteDesktop: pipeline has no unlinked sink pad\n");
      return FALSE;
    }

  src = meta_remote_desktop_src_new (DEFAULT_FRAMERATE,
                                     session->width,
                                     session->height);
  if (src == NULL)
    {
      meta_warning ("MetaRemoteDesktop: Can't create source element\n");
      return FALSE;
    }
  gst_bin_add (GST_BIN (pipeline), GST_ELEMENT (src));

  src_pad = gst_element_get_static_pad (GST_ELEMENT (src), "src");
  if (!src_pad)
    {
      meta_warning ("MetaRemoteDesktop: can't get src pad link into pipeline\n");
      return FALSE;
    }

  if (gst_pad_link (src_pad, sink_pad) != GST_PAD_LINK_OK)
    {
      meta_warning ("MetaRemoteDesktop: can't link to sink pad\n");
      return FALSE;
    }

  session->src = GST_ELEMENT (src);

  return TRUE;
}

static gboolean
meta_remote_desktop_session_open_pipeline (MetaRemoteDesktopSession *session)
{
  g_autoptr(GstElement) pipeline;
  GstBus *bus;
  GError *error = NULL;

  pipeline = gst_pipeline_new (NULL);
  if (!pipeline)
    {
      meta_warning ("MetaRemoteDesktop: Couldn't start pinos sink: %s\n",
                    error->message);
      return FALSE;
    }

  GstElement *pinossink = gst_element_factory_make ("pinossink", NULL);
  gst_bin_add (GST_BIN (pipeline), pinossink);

  if (!meta_remote_desktop_session_add_source (session, pipeline))
    {
      meta_warning ("MetaRemoteDesktop: Couldn't add video source\n");
      return FALSE;
    }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus,
                     meta_remote_desktop_session_pipeline_bus_watch,
                     session);
  gst_object_unref (bus);

  session->pipeline = g_steal_pointer (&pipeline);

  g_object_ref (session);

  return TRUE;
}

static void
meta_remote_desktop_session_close_pipeline (MetaRemoteDesktopSession *session)
{
  gst_element_send_event (session->pipeline, gst_event_new_eos ());
}

static void
meta_remote_desktop_session_record_frame (MetaRemoteDesktopSession *session,
                                          GstClockTime              now)
{
  GstBuffer *buffer;
  uint8_t *data;
  size_t size;

  size = session->width * session->height * 4;

  /* TODO: Disable using hw planes if we rely on read_pixels. */

  data = g_malloc (size);
  cogl_framebuffer_read_pixels (cogl_get_draw_framebuffer (),
                                0,
                                0,
                                session->width,
                                session->height,
                                CLUTTER_CAIRO_FORMAT_ARGB32,
                                data);

  buffer = gst_buffer_new ();
  gst_buffer_insert_memory (buffer, -1,
                            gst_memory_new_wrapped (0, data, size, 0,
                                                    size, data, g_free));

  meta_remote_desktop_src_add_buffer (META_REMOTE_DESKTOP_SRC (session->src),
                                      buffer);
  gst_buffer_unref (buffer);
}

static void
meta_remote_desktop_session_on_stage_paint (ClutterActor             *actor,
                                            MetaRemoteDesktopSession *session)
{
  GstClock *clock;
  GstClockTime now;
  GstClockTime interval_threshold;

  g_return_if_fail (session->pipeline);

  clock = gst_element_get_clock (GST_ELEMENT (session->src));
  if (!clock)
    return;

  now = gst_clock_get_time (clock);

  /* Drop frames if the interval since the last frame is less than 75% of
   * the desired frame interval.
   */
  interval_threshold = gst_util_uint64_scale_int (GST_SECOND,
                                                  3,
                                                  4 * DEFAULT_FRAMERATE);
  if (GST_CLOCK_TIME_IS_VALID (session->last_frame_time) &&
      now - session->last_frame_time < interval_threshold)
    return;

  meta_remote_desktop_session_record_frame (session, now);
}

gboolean
meta_remote_desktop_session_start (MetaRemoteDesktopSession *session)
{
  if (!meta_remote_desktop_session_open_pipeline (session))
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
  meta_remote_desktop_session_close_pipeline (session);

  g_signal_handlers_disconnect_by_func (session->stage,
                                        (gpointer) meta_remote_desktop_session_on_stage_paint,
                                        session);
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
  MetaBackend *backend = meta_get_backend ();
  ClutterActor *stage = meta_backend_get_stage (backend);
  ClutterActorBox allocation;

  session->stage = stage;
  clutter_actor_get_allocation_box (session->stage, &allocation);
  session->width = (int)(0.5 + allocation.x2 - allocation.x1);
  session->height = (int)(0.5 + allocation.y2 - allocation.y1);

  session->last_frame_time = GST_CLOCK_TIME_NONE;
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
