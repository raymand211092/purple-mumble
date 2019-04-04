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

#ifndef MUMBLE_OUTPUT_STREAM_H
#define MUMBLE_OUTPUT_STREAM_H

#include <glib.h>
#include <gio/gio.h>
#include "mumble-message.h"

#define MUMBLE_TYPE_OUTPUT_STREAM mumble_output_stream_get_type()

#define MUMBLE_OUTPUT_STREAM_ERROR g_quark_from_static_string("mumble-output-stream-quark")

typedef struct _MumbleOutputStreamClass {
  GFilterOutputStreamClass parent;
} MumbleOutputStreamClass;

typedef struct _MumbleOutputStream {
  GFilterOutputStream parent;
} MumbleOutputStream;

gboolean mumble_output_stream_write_message_finish(MumbleOutputStream *stream, GAsyncResult *result, GError **error);
void mumble_output_stream_write_message_async(MumbleOutputStream *stream, MumbleMessage *message, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer userData);
GOutputStream *mumble_output_stream_new(GOutputStream *baseStream);
GType mumble_output_stream_get_type();

#endif
