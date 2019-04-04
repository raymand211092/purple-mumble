/*
 * purple-mumble -- Mumble protocol plugin for libpurple
 * Copyright (C) 2018-2019  Petteri Pitkänen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <glib/gi18n.h>
#include "mumble-input-stream.h"

struct _MumbleInputStreamPrivate {
  guint8 *buffer;
  gint offset;
};

G_DEFINE_TYPE_WITH_PRIVATE(MumbleInputStream, mumble_input_stream, G_TYPE_FILTER_INPUT_STREAM)

static void on_read(GObject *object, GAsyncResult *result, gpointer userData);
static void start_read(MumbleInputStream *stream, GTask *task, gint count);
static void finalize(GObject *object);

MumbleMessage *mumble_input_stream_read_message_finish(MumbleInputStream *stream, GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

void mumble_input_stream_read_message_async(MumbleInputStream *stream, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer callbackData) {
  GTask *task = g_task_new(stream, cancellable, callback, callbackData);
  start_read(stream, task, 6);
}

GInputStream *mumble_input_stream_new(GInputStream *baseStream) {
  return g_object_new(MUMBLE_TYPE_INPUT_STREAM, "base-stream", baseStream, NULL);
}

static void mumble_input_stream_init(MumbleInputStream *stream) {
  MumbleInputStreamPrivate *priv = mumble_input_stream_get_instance_private(stream);

  priv->buffer = g_malloc(256 * 1024); // TODO Check this properly
  priv->offset = 0;
}

static void mumble_input_stream_class_init(MumbleInputStreamClass *mumbleInputStreamClass) {
  GObjectClass *objectClass = G_OBJECT_CLASS(mumbleInputStreamClass);
  objectClass->finalize = finalize;
}

static void start_read(MumbleInputStream *stream, GTask *task, gint count) {
  MumbleInputStreamPrivate *priv = mumble_input_stream_get_instance_private(stream);
  g_input_stream_read_async(stream, priv->buffer + priv->offset, count, G_PRIORITY_DEFAULT, g_task_get_cancellable(task), on_read, task);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer userData) {
  GTask *task = G_TASK(userData);
  MumbleInputStream *stream = g_task_get_source_object(task);
  MumbleInputStreamPrivate *priv = mumble_input_stream_get_instance_private(stream);
  GInputStream *baseStream = G_FILTER_INPUT_STREAM(stream)->base_stream;

  GError *error = NULL;
  gssize count = g_input_stream_read_finish(stream, result, &error);

  if (count <= 0) {
    if (!error) {
      error = g_error_new(MUMBLE_INPUT_STREAM_ERROR, MUMBLE_INPUT_STREAM_ERROR_SERVER_CLOSED_CONNECTION, _("Server closed the connection"));
    }

    g_task_return_error(task, error);
    g_object_unref(task);

    return;
  }

  priv->offset += count;
  MumbleMessage *message = mumble_message_read(priv->buffer, priv->offset);

  if (message) {
    priv->offset = 0;

    g_task_return_pointer(task, message, mumble_message_free);
    g_object_unref(task);
  } else {
    gint remaining = mumble_message_get_minimum_length(priv->buffer, priv->offset) - priv->offset;
    start_read(stream, task, remaining);
  }
}

static void finalize(GObject *object) {
  MumbleInputStreamPrivate *priv = mumble_input_stream_get_instance_private(object);

  g_free(priv->buffer);

  G_OBJECT_CLASS(mumble_input_stream_parent_class)->finalize(object);
}
