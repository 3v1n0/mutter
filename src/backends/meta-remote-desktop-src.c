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
 *
 * Based on shell-recorder-src.c from gnome-shell.
 */

#include "config.h"

#include "backends/meta-remote-desktop-src.h"

#define GST_USE_UNSTABLE_API
#include <gst/base/gstpushsrc.h>

struct _MetaRemoteDesktopSrc
{
  GstPushSrc parent;

  GstCaps *caps;

  GMutex queue_lock;
  GCond queue_cond;
  GQueue *queue;
  gboolean eos;
  gboolean flushing;
};

#define meta_remote_desktop_src_parent_class parent_class
G_DEFINE_TYPE (MetaRemoteDesktopSrc,
               meta_remote_desktop_src,
               GST_TYPE_PUSH_SRC);

static void
meta_remote_desktop_src_close (MetaRemoteDesktopSrc *src)
{
  g_mutex_lock (&src->queue_lock);
  src->eos = TRUE;
  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);
}

static gboolean
meta_remote_desktop_src_send_event (GstElement *element,
                                    GstEvent   *event)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (element);

  switch (GST_EVENT_TYPE (event))
    {
    case GST_EVENT_EOS:
      meta_remote_desktop_src_close (src);
      gst_event_unref (event);
      return TRUE;

    default:
      return GST_CALL_PARENT_WITH_DEFAULT (GST_ELEMENT_CLASS,
                                           send_event,
                                           (element, event),
                                           FALSE);
    }
}

static gboolean
meta_remote_desktop_src_negotiate (GstBaseSrc *base_src)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (base_src);

  return gst_base_src_set_caps (base_src, src->caps);
}

static gboolean
meta_remote_desktop_src_unlock (GstBaseSrc *base_src)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (base_src);

  g_mutex_lock (&src->queue_lock);
  src->flushing = TRUE;
  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);

  return TRUE;
}

static gboolean
meta_remote_desktop_src_unlock_stop (GstBaseSrc *base_src)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (base_src);

  g_mutex_lock (&src->queue_lock);
  src->flushing = FALSE;
  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);

  return TRUE;
}

static gboolean
meta_remote_desktop_src_start (GstBaseSrc *base_src)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (base_src);

  g_mutex_lock (&src->queue_lock);
  src->flushing = FALSE;
  src->eos = FALSE;
  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);

  return TRUE;
}

static gboolean
meta_remote_desktop_src_stop (GstBaseSrc *base_src)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (base_src);

  g_mutex_lock (&src->queue_lock);
  src->flushing = TRUE;
  src->eos = FALSE;
  g_queue_foreach (src->queue, (GFunc) gst_buffer_unref, NULL);
  g_queue_clear (src->queue);
  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);

  return TRUE;
}

static GstFlowReturn
meta_remote_desktop_src_create (GstPushSrc  *push_src,
                                GstBuffer  **buffer_out)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (push_src);
  GstBuffer *buffer;

  g_mutex_lock (&src->queue_lock);
  while (TRUE)
    {
      if (src->flushing)
        {
          g_mutex_unlock (&src->queue_lock);
          return GST_FLOW_FLUSHING;
        }

      buffer = g_queue_pop_head (src->queue);
      if (buffer)
        break;

      if (src->eos)
        {
          g_mutex_unlock (&src->queue_lock);
          return GST_FLOW_EOS;
        }

      g_cond_wait (&src->queue_cond, &src->queue_lock);
    }
  g_mutex_unlock (&src->queue_lock);

  *buffer_out = buffer;

  return GST_FLOW_OK;
}

void
meta_remote_desktop_src_add_buffer (MetaRemoteDesktopSrc *src,
                                    GstBuffer            *buffer)
{
  GstBuffer *old_buffer = NULL;

  g_mutex_lock (&src->queue_lock);
  g_queue_push_tail (src->queue, gst_buffer_ref (buffer));

  /* Drop too old buffers because they'll be out of date anyway. */
  if (g_queue_get_length (src->queue) > 2)
    {
      old_buffer = g_queue_pop_head (src->queue);
    }

  g_cond_signal (&src->queue_cond);
  g_mutex_unlock (&src->queue_lock);

  if (old_buffer)
    gst_buffer_unref (old_buffer);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  gst_element_register (plugin,
                        "metaremotedesktopsrc",
                        GST_RANK_NONE,
                        META_TYPE_REMOTE_DESKTOP_SRC);
  return TRUE;
}

static void
meta_remote_desktop_src_register (void)
{
  static gboolean registered = FALSE;

  if (registered)
    return;

  gst_plugin_register_static (GST_VERSION_MAJOR, GST_VERSION_MINOR,
                              "metaremotedesktop",
                              "MetaRemoteDesktop plugin",
                              plugin_init,
                              "0.1",
                              "LGPL",
                              "mutter", "mutter",
                              "http://git.gnome.org/browse/mutter");
  registered = TRUE;
}

MetaRemoteDesktopSrc *
meta_remote_desktop_src_new (int frames_per_second,
                             int width,
                             int height)
{
  GstElement *element;
  MetaRemoteDesktopSrc *src;

  meta_remote_desktop_src_register ();

  element = gst_element_factory_make ("metaremotedesktopsrc", NULL);
  if (!element)
    return NULL;

  src = META_REMOTE_DESKTOP_SRC (element);

  src->caps =
    gst_caps_new_simple ("video/x-raw",
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
                         "format", G_TYPE_STRING, "BGRx",
#else
                         "format", G_TYPE_STRING, "xRGB",
#endif
                         "framerate", GST_TYPE_FRACTION, frames_per_second, 1,
                         "width", G_TYPE_INT, width,
                         "height", G_TYPE_INT, height,
                         NULL);

  return src;
}

static void
meta_remote_desktop_src_init (MetaRemoteDesktopSrc *src)
{
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  src->queue = g_queue_new ();
  g_mutex_init (&src->queue_lock);
  g_cond_init (&src->queue_cond);
}

static void
meta_remote_desktop_src_finalize (GObject *object)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (object);

  g_clear_pointer (&src->caps, gst_mini_object_unref);

  g_queue_free_full (src->queue, (GDestroyNotify) gst_buffer_unref);
  g_mutex_clear (&src->queue_lock);
  g_cond_clear (&src->queue_cond);

  G_OBJECT_CLASS (meta_remote_desktop_src_parent_class)->finalize (object);
}

static void
meta_remote_desktop_src_class_init (MetaRemoteDesktopSrcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS (klass);

  static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
                             GST_PAD_SRC,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);

  gst_element_class_add_pad_template (element_class,
                                      gst_static_pad_template_get (&src_template));

  gst_element_class_set_details_simple (element_class,
                                        "MetaRemoteDesktopSrc",
                                        "Generic/Src",
                                        "Remote desktop screen pipeline source",
                                        "Jonas Ådahl <jadahl@redhat.com>");

  object_class->finalize = meta_remote_desktop_src_finalize;

  element_class->send_event = meta_remote_desktop_src_send_event;

  base_src_class->negotiate = meta_remote_desktop_src_negotiate;
  base_src_class->unlock = meta_remote_desktop_src_unlock;
  base_src_class->unlock_stop = meta_remote_desktop_src_unlock_stop;
  base_src_class->start = meta_remote_desktop_src_start;
  base_src_class->stop = meta_remote_desktop_src_stop;

  push_src_class->create = meta_remote_desktop_src_create;
}
