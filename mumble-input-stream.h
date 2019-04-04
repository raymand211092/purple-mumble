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

#ifndef MUMBLE_INPUT_STREAM_H
#define MUMBLE_INPUT_STREAM_H

#include <glib.h>
#include <gio/gio.h>
#include "mumble-message.h"

#define MUMBLE_TYPE_INPUT_STREAM mumble_input_stream_get_type()

#define MUMBLE_INPUT_STREAM_ERROR g_quark_from_static_string("mumble-input-stream-quark")

typedef struct _MumbleInputStreamPrivate MumbleInputStreamPrivate;

typedef struct _MumbleInputStreamClass {
  GFilterInputStreamClass parent;
} MumbleInputStreamClass;

typedef struct _MumbleInputStream {
  GFilterInputStream parent;
} MumbleInputStream;

typedef enum {
  MUMBLE_INPUT_STREAM_ERROR_SERVER_CLOSED_CONNECTION,
  MUMBLE_INPUT_STREAM_ERROR_MAX_MESSAGE_SIZE_EXCEEDED,
} MumbleInputStreamError;

MumbleMessage *mumble_input_stream_read_message_finish(MumbleInputStream *stream, GAsyncResult *result, GError **error);
void mumble_input_stream_read_message_async(MumbleInputStream *stream, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer userData);
GInputStream *mumble_input_stream_new(GInputStream *baseStream);
GType mumble_input_stream_get_type();

#endif
