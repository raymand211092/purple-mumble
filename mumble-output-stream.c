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

#include <purple.h>
#include "mumble-output-stream.h"

G_DEFINE_TYPE(MumbleOutputStream, mumble_output_stream, PURPLE_TYPE_QUEUED_OUTPUT_STREAM)

static void on_written(GObject *source, GAsyncResult *result, gpointer data);

gboolean mumble_output_stream_write_message_finish(MumbleOutputStream *stream, GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

void mumble_output_stream_write_message_async(MumbleOutputStream *stream, MumbleMessage *message, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer callback_data) {
  guint8 *buffer = g_malloc(64 * 1024); // TODO Check this properly
  gint count = mumble_message_write(message, buffer);
  GBytes *bytes = g_bytes_new_take(buffer, count);

  mumble_message_free(message);

  GTask *task = g_task_new(stream, cancellable, callback, callback_data);
  purple_queued_output_stream_push_bytes_async(stream, bytes, G_PRIORITY_DEFAULT, cancellable, on_written, task);

  g_bytes_unref(bytes);
}

GOutputStream *mumble_output_stream_new(GOutputStream *base_stream) {
  return g_object_new(MUMBLE_TYPE_OUTPUT_STREAM, "base-stream", base_stream, NULL);
}

static void mumble_output_stream_init(MumbleOutputStream *stream) {
}

static void mumble_output_stream_class_init(MumbleOutputStreamClass *mumble_output_stream_class) {
}

static void on_written(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = G_TASK(data);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}
