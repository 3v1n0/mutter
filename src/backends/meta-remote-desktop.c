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

#define _GNU_SOURCE

#include "config.h"

#include "backends/meta-remote-desktop.h"

#include <fcntl.h>
#include <gst/gst.h>
#include <gst/allocators/gstfdmemory.h>
#include <stdlib.h>
#include <string.h>

#include <meta/errors.h>
#include <meta/meta-backend.h>

#include "meta-dbus-remote-desktop.h"
#include "backends/meta-remote-desktop-session.h"

#define META_REMOTE_DESKTOP_DBUS_SERVICE "org.gnome.Mutter.RemoteDesktop"
#define META_REMOTE_DESKTOP_DBUS_PATH "/org/gnome/Mutter/RemoteDesktop"

struct _MetaRemoteDesktop
{
  MetaDBusRemoteDesktopSkeleton parent;

  int dbus_name_id;
  GstAllocator *fd_allocator;

  MetaRemoteDesktopSession *session;
};

static void meta_remote_desktop_init_iface (MetaDBusRemoteDesktopIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaRemoteDesktop,
                         meta_remote_desktop,
                         META_DBUS_TYPE_REMOTE_DESKTOP_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_REMOTE_DESKTOP,
                                                meta_remote_desktop_init_iface));

static int
tmpfile_create (size_t size)
{
  char filename[] = "/dev/shm/tmpmetaremote.XXXXXX";
  int fd;

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1)
    {
      meta_warning ("Failed to create temporary file: %s\n", strerror (errno));
      return -1;
    }
  unlink (filename);

  if (ftruncate (fd, size) == -1)
    {
      meta_warning ("Failed to truncate temporary file: %s\n",
                    strerror (errno));
      close (fd);
      return -1;
    }

  return fd;
}

GstBuffer *
meta_remote_desktop_try_create_tmpfile_gst_buffer (MetaRemoteDesktop *rd,
                                                   size_t             size)
{
  GstBuffer *buffer;
  GstMemory *memory;
  int fd;

  if (!rd->fd_allocator)
    return NULL;

  fd = tmpfile_create (size);
  if (fd == -1)
    return NULL;

  memory = gst_fd_allocator_alloc (rd->fd_allocator,
                                   fd,
                                   size,
                                   GST_FD_MEMORY_FLAG_NONE);
  if (!memory)
    {
      close (fd);
      return NULL;
    }

  buffer = gst_buffer_new ();
  gst_buffer_append_memory (buffer, memory);

  return buffer;
}

static gboolean
meta_remote_desktop_handle_start (MetaDBusRemoteDesktop *skeleton,
                                  GDBusMethodInvocation *invocation)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (skeleton);
  MetaRemoteDesktopSession *session;
  const char *stream_id;

  fprintf (stderr, "RD: start\n");

  session = meta_remote_desktop_session_new (rd);

  if (!meta_remote_desktop_session_start (session))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to initiate remote desktop");
      return TRUE;
    }

  rd->session = session;

  stream_id = meta_remote_desktop_session_get_stream_id (session);
  meta_dbus_remote_desktop_complete_start (skeleton,
                                           invocation,
                                           stream_id);

  return TRUE;
}

static gboolean
meta_remote_desktop_handle_stop (MetaDBusRemoteDesktop *skeleton,
                                 GDBusMethodInvocation *invocation)
{
  MetaRemoteDesktop *rd = META_REMOTE_DESKTOP (skeleton);

  fprintf (stderr, "RD: stop\n");

  if (!rd->session)
    {
      g_object_unref (session);
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No remote desktop session to stop");
      return TRUE;
    }

  if (rd->session && meta_remote_desktop_session_is_running (rd->session))
    meta_remote_desktop_session_stop (rd->session);
  g_clear_object (&rd->session);

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

  rd->fd_allocator = gst_fd_allocator_new ();
  if (!rd->fd_allocator)
    meta_warning ("Missing fdmemory gstreamer plugin, fallback to malloc\n");
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
