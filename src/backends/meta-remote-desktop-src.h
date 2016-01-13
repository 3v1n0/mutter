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

#ifndef META_REMOTE_DESKTOP_SRC_H
#define META_REMOTE_DESKTOP_SRC_H

#include <glib-object.h>
#define GST_USE_UNSTABLE_API
#include <gst/base/gstpushsrc.h>

typedef struct _MetaRemoteDesktopSrc MetaRemoteDesktopSrc;

#define META_TYPE_REMOTE_DESKTOP_SRC (meta_remote_desktop_src_get_type ())
G_DECLARE_FINAL_TYPE (MetaRemoteDesktopSrc,
                      meta_remote_desktop_src,
                      META, REMOTE_DESKTOP_SRC,
                      GstPushSrc);

MetaRemoteDesktopSrc * meta_remote_desktop_src_new (int frames_per_second,
                                                    int width,
                                                    int height);

void meta_remote_desktop_src_add_buffer (MetaRemoteDesktopSrc *src,
                                         GstBuffer            *buffer);

#endif /* META_REMOTE_DESKTOP_SRC_H */
