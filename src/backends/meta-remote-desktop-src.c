/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
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

#include <errno.h>
#include <pinos/client/pinos.h>
#include <pinos/client/sig.h>
#include <spa/format-builder.h>
#include <spa/format-utils.h>
#include <spa/props.h>
#include <spa/type-map.h>
#include <spa/video/format-utils.h>
#include <stdint.h>
#include <sys/mman.h>

#include "clutter/clutter-mutter.h"
#include "mutter/meta/boxes.h"

enum
{
  PROP_0,

  PROP_STREAM_ID,
  PROP_RECT,
  PROP_FRAMERATE,
};

typedef struct _MetaSpaType
{
  uint32_t format;
  uint32_t props;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeVideoFormat video_format;
} MetaSpaType;

typedef struct _MetaPinosSource
{
  GSource base;

  PinosLoop *pinos_loop;
} MetaPinosSource;

struct _MetaRemoteDesktopSrc
{
  GObject parent;

  PinosContext *pinos_context;
  MetaPinosSource *pinos_source;
  PinosListener on_state_changed_listener;

  PinosStream *pinos_stream;
  PinosListener on_stream_state_changed_listener;
  PinosListener on_stream_format_changed_listener;

  char *stream_id;
  MetaRectangle rect;

  MetaSpaType spa_type;
  uint8_t params_buffer[1024];
  SpaVideoInfoRaw video_format;

  gint64 last_frame_timestamp_us;
};

G_DEFINE_TYPE (MetaRemoteDesktopSrc,
               meta_remote_desktop_src,
               G_TYPE_OBJECT);

#define PROP(f, key, type, ...)                                         \
          SPA_POD_PROP (f, key, 0, type, 1, __VA_ARGS__)
#define PROP_U_MM(f, key, type, ...)                                    \
          SPA_POD_PROP (f, key, (SPA_POD_PROP_FLAG_UNSET |              \
                                 SPA_POD_PROP_RANGE_MIN_MAX),           \
                        type, 3, __VA_ARGS__)

static void
record_frame (MetaRemoteDesktopSrc *src,
              ClutterStage         *stage,
              uint8_t              *data)
{
  clutter_stage_capture_into (stage, FALSE,
                              &(cairo_rectangle_int_t) {
                                .x = src->rect.x,
                                .y = src->rect.y,
                                .width = src->rect.width,
                                .height = src->rect.height
                              },
                              data);
}

void
meta_remote_desktop_src_maybe_record_frame (MetaRemoteDesktopSrc *src,
                                            ClutterStage         *stage)
{
  uint32_t buffer_id;
  SpaBuffer *buffer;
  uint8_t *map = NULL;
  uint8_t *data;
  gint64 now_us;

  now_us = g_get_monotonic_time ();
  if (src->last_frame_timestamp_us != 0 &&
      (now_us - src->last_frame_timestamp_us <
       ((1000000 * src->video_format.max_framerate.denom) /
        src->video_format.max_framerate.num)))
    return;

  if (!src->pinos_stream)
    return;

  buffer_id = pinos_stream_get_empty_buffer (src->pinos_stream);
  if (buffer_id == SPA_ID_INVALID)
    return;

  buffer = pinos_stream_peek_buffer (src->pinos_stream, buffer_id);

  if (buffer->datas[0].type == src->spa_type.data.MemFd)
    {
      map = mmap (NULL, buffer->datas[0].maxsize + buffer->datas[0].mapoffset,
                  PROT_READ | PROT_WRITE, MAP_SHARED,
                  buffer->datas[0].fd, 0);
      if (map == MAP_FAILED)
        {
          g_warning ("Failed to mmap pinos stream buffer: %s\n", strerror (errno));
          return;
        }

      data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);
    }
  else if (buffer->datas[0].type == src->spa_type.data.MemPtr)
    {
      data = buffer->datas[0].data;
    }
  else
    {
      return;
    }

  record_frame (src, stage, data);
  src->last_frame_timestamp_us = now_us;

  if (map)
    munmap (map, buffer->datas[0].maxsize);

  pinos_stream_send_buffer (src->pinos_stream, buffer_id);
}

static void
on_stream_state_changed (PinosListener  *listener,
                         PinosStream    *stream)
{
  switch (stream->state)
    {
    case PINOS_STREAM_STATE_PAUSED:
      break;
    case PINOS_STREAM_STATE_STREAMING:
      break;
    default:
      g_warning ("Unhandled pinos stream state: %d", stream->state);
      break;
    }
}

static void
on_stream_format_changed (PinosListener  *listener,
                          PinosStream    *stream,
                          SpaFormat      *format)
{
  MetaRemoteDesktopSrc *src =
    SPA_CONTAINER_OF (listener,
                      MetaRemoteDesktopSrc,
                      on_stream_format_changed_listener);
  PinosContext *pinos_context = src->pinos_context;
  SpaTypeAllocParamBuffers *alloc_param_buffers;
  SpaTypeAllocParamMetaEnable *alloc_param_meta_enable;
  SpaPODBuilder pod_builder = { NULL };
  SpaPODFrame object_frame;
  SpaPODFrame prop_frame;
  SpaAllocParam *alloc_params[2];
  const int bpp = 4;

  if (!format)
    {
      pinos_stream_finish_format (stream, SPA_RESULT_OK, NULL, 0);
      return;
    }

  spa_format_video_raw_parse (format,
                              &src->video_format,
                              &src->spa_type.format_video);

  spa_pod_builder_init (&pod_builder,
                        src->params_buffer,
                        sizeof (src->params_buffer));

  alloc_param_buffers = &pinos_context->type.alloc_param_buffers;
  spa_pod_builder_object (&pod_builder, &object_frame, 0,
                          alloc_param_buffers->Buffers,
                          PROP (&prop_frame, alloc_param_buffers->size,
                                SPA_POD_TYPE_INT,
                                (src->video_format.size.width *
                                 src->video_format.size.height *
                                 bpp)),
                          PROP (&prop_frame, alloc_param_buffers->stride,
                                SPA_POD_TYPE_INT,
                                src->video_format.size.width * bpp),
                          PROP_U_MM (&prop_frame, alloc_param_buffers->buffers,
                                     SPA_POD_TYPE_INT,
                                     2, 2, 2),
                          PROP (&prop_frame, alloc_param_buffers->align,
                                SPA_POD_TYPE_INT,
                                16));
  alloc_params[0] = SPA_POD_BUILDER_DEREF (&pod_builder, object_frame.ref,
                                           SpaAllocParam);

  alloc_param_meta_enable = &pinos_context->type.alloc_param_meta_enable;
  spa_pod_builder_object (&pod_builder, &object_frame, 0,
                          alloc_param_meta_enable->MetaEnable,
                          PROP (&prop_frame, alloc_param_meta_enable->type,
                                SPA_POD_TYPE_ID,
                                pinos_context->type.meta.Header),
                          PROP (&prop_frame, alloc_param_meta_enable->size,
                                SPA_POD_TYPE_INT,
                                sizeof (SpaMetaHeader)));
  alloc_params[1] = SPA_POD_BUILDER_DEREF (&pod_builder, object_frame.ref,
                                           SpaAllocParam);

  pinos_stream_finish_format (stream, SPA_RESULT_OK, alloc_params, 2);
}

static PinosStream *
create_pinos_stream (MetaRemoteDesktopSrc *src,
                     GError              **error)
{
  PinosProperties *properties;
  PinosStream *pinos_stream;
  SpaFormat *format;
  uint8_t buffer[1024];
  SpaPODBuilder pod_builder = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  SpaPODFrame format_frame;
  SpaPODFrame prop_frame;
  MetaSpaType *spa_type = &src->spa_type;

  properties = pinos_properties_new ("gnome.remote_desktop.stream_id",
                                     src->stream_id,
                                     NULL);

  pinos_stream = pinos_stream_new (src->pinos_context,
                                   "meta-remote-desktop-src",
                                   properties);

  spa_pod_builder_format (&pod_builder, &format_frame,
                          spa_type->format,
                          spa_type->media_type.video,
                          spa_type->media_subtype.raw,
                          PROP (&prop_frame,
                                spa_type->format_video.format,
                                SPA_POD_TYPE_ID, spa_type->video_format.BGRx),
                          PROP_U_MM (&prop_frame,
                                     spa_type->format_video.size,
                                     SPA_POD_TYPE_RECTANGLE,
                                     src->rect.width, src->rect.height,
                                     src->rect.width, src->rect.height,
                                     src->rect.width, src->rect.height),
                          PROP (&prop_frame,
                                spa_type->format_video.framerate,
                                SPA_POD_TYPE_FRACTION,
                                0, 1),
                          PROP_U_MM (&prop_frame,
                                     spa_type->format_video.max_framerate,
                                     SPA_POD_TYPE_FRACTION,
                                     60, 1,
                                     1, 1,
                                     60, 1));
  format = SPA_POD_BUILDER_DEREF (&pod_builder, format_frame.ref, SpaFormat);

  pinos_signal_add (&pinos_stream->state_changed,
                    &src->on_stream_state_changed_listener,
                    on_stream_state_changed);
  pinos_signal_add (&pinos_stream->format_changed,
                    &src->on_stream_format_changed_listener,
                    on_stream_format_changed);

  if (!pinos_stream_connect (pinos_stream,
                             PINOS_DIRECTION_OUTPUT,
                             PINOS_STREAM_MODE_BUFFER,
                             NULL,
                             PINOS_STREAM_FLAG_NONE,
                             1, &format))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not connect");
      return NULL;
    }

  return pinos_stream;
}

static void
on_state_changed (PinosListener *listener,
                  PinosContext  *pinos_context)
{
  MetaRemoteDesktopSrc *src = SPA_CONTAINER_OF (listener,
                                                MetaRemoteDesktopSrc,
                                                on_state_changed_listener);
  PinosStream *pinos_stream;
  GError *error = NULL;

  switch (pinos_context->state)
    {
    case PINOS_CONTEXT_STATE_ERROR:
      g_warning ("pinos context error: %s\n", pinos_context->error);
      break;

    case PINOS_CONTEXT_STATE_CONNECTED:
      pinos_stream = create_pinos_stream (src, &error);
      if (!pinos_stream)
        {
          g_warning ("Could not create pinos stream: %s", error->message);
          g_error_free (error);
        }
      else
        {
          src->pinos_stream = pinos_stream;
        }
      break;

    default:
      g_warning ("Unhandled pinos context state: %d", pinos_context->state);
      break;
    }
}

MetaRemoteDesktopSrc *
meta_remote_desktop_src_new (const char    *stream_id,
                             MetaRectangle *rect)
{
  return g_object_new (META_TYPE_REMOTE_DESKTOP_SRC,
                       "stream-id", stream_id,
                       "rect", rect,
                       NULL);
}

static gboolean
pinos_loop_source_prepare (GSource *base,
                           int     *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pinos_loop_source_dispatch (GSource    *source,
                            GSourceFunc callback,
                            gpointer    user_data)
{
  MetaPinosSource *pinos_source = (MetaPinosSource *) source;
  SpaResult result;

  result = pinos_loop_iterate (pinos_source->pinos_loop, 0);
  if (result == SPA_RESULT_ERRNO)
    g_warning ("pinos_loop_iterate failed: %s", strerror (errno));
  else if (result != SPA_RESULT_OK)
    g_warning ("pinos_loop_iterate failed: %d", result);

  return TRUE;
}

static void
pinos_loop_source_finalize (GSource *source)
{
  MetaPinosSource *pinos_source = (MetaPinosSource *) source;

  pinos_loop_leave (pinos_source->pinos_loop);
  pinos_loop_destroy (pinos_source->pinos_loop);
}

static GSourceFuncs pinos_source_funcs =
{
  pinos_loop_source_prepare,
  NULL,
  pinos_loop_source_dispatch,
  pinos_loop_source_finalize
};

static void
init_spa_type (MetaSpaType *type,
               SpaTypeMap  *map)
{
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
}

static MetaPinosSource *
create_pinos_source (void)
{
  MetaPinosSource *pinos_source;

  pinos_source =
    (MetaPinosSource *) g_source_new (&pinos_source_funcs,
                                      sizeof (MetaPinosSource));
  pinos_source->pinos_loop = pinos_loop_new ();
  g_source_add_unix_fd (&pinos_source->base,
                        pinos_loop_get_fd (pinos_source->pinos_loop),
                        G_IO_IN | G_IO_ERR);

  pinos_loop_enter (pinos_source->pinos_loop);
  g_source_attach (&pinos_source->base, NULL);

  return pinos_source;
}

static void
meta_remote_desktop_src_constructed (GObject *object)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (object);

  src->pinos_source = create_pinos_source ();
  src->pinos_context = pinos_context_new (src->pinos_source->pinos_loop,
                                          "meta-remote-desktop-src",
                                          NULL);

  pinos_signal_add (&src->pinos_context->state_changed,
                    &src->on_state_changed_listener,
                    on_state_changed);

  init_spa_type (&src->spa_type, src->pinos_context->type.map);

  pinos_context_connect (src->pinos_context,
                         PINOS_CONTEXT_FLAG_NO_REGISTRY);
}

static void
meta_remote_desktop_src_finalize (GObject *object)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (object);

  pinos_context_destroy (src->pinos_context);
  g_source_destroy (&src->pinos_source->base);
  g_clear_pointer (&src->stream_id, g_free);

  G_OBJECT_CLASS (meta_remote_desktop_src_parent_class)->finalize (object);
}

static void
meta_remote_desktop_src_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (object);

  switch (prop_id)
    {
    case PROP_STREAM_ID:
      src->stream_id = g_strdup (g_value_get_string (value));
      break;
    case PROP_RECT:
      src->rect = *(MetaRectangle *) g_value_get_boxed (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_remote_desktop_src_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  MetaRemoteDesktopSrc *src = META_REMOTE_DESKTOP_SRC (object);

  switch (prop_id)
    {
    case PROP_STREAM_ID:
      g_value_set_string (value, src->stream_id);
      break;
    case PROP_RECT:
      g_value_set_boxed (value, &src->rect);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_remote_desktop_src_init (MetaRemoteDesktopSrc *src)
{
}

static void
meta_remote_desktop_src_class_init (MetaRemoteDesktopSrcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = meta_remote_desktop_src_constructed;
  object_class->finalize = meta_remote_desktop_src_finalize;
  object_class->set_property = meta_remote_desktop_src_set_property;
  object_class->get_property = meta_remote_desktop_src_get_property;

  g_object_class_install_property (object_class,
                                   PROP_STREAM_ID,
                                   g_param_spec_string ("stream-id",
                                                        "stream-id",
                                                        "Unique stream ID",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class,
                                   PROP_RECT,
                                   g_param_spec_boxed ("rect",
                                                       "rect",
                                                       "area on stage of video src",
                                                       META_TYPE_RECTANGLE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));
}
