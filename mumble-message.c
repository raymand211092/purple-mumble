/*
 * purple-mumble -- Mumble protocol plugin for libpurple
 * Copyright (C) 2018-2020  Petteri Pitkänen
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

#include "mumble-message.h"

G_DEFINE_BOXED_TYPE(MumbleMessage, mumble_message, mumble_message_copy, mumble_message_free)

MumbleMessage *mumble_message_read(guint8 *buffer, guint length) {
  MumbleMessage *message = NULL;
  if (length >= 6) {
    guint type = buffer[1];
    guint message_length = mumble_message_get_minimum_bytes(buffer, length);

    if (length >= message_length) {
      GByteArray *payload = g_byte_array_new();
      g_byte_array_append(payload, buffer + 6, message_length - 6);
      message = mumble_message_new(type, payload);
    }
  }
  return message;
}

guint mumble_message_get_minimum_bytes(guint8 *buffer, guint partial_length) {
  guint minimum_bytes;
  if (partial_length < 6) {
    minimum_bytes = 6;
  } else {
    guint payload_length = (buffer[2] << 24) | (buffer[3] << 16) | (buffer[4] << 8) | buffer[5];
    minimum_bytes = 6 + payload_length;
  }
  return minimum_bytes;
}

gint mumble_message_write(MumbleMessage *message, guint8 *buffer) {
  gint packed_size = message->payload->len;

  buffer[0] = 0;
  buffer[1] = message->type;
  buffer[2] = packed_size >> 24;
  buffer[3] = packed_size >> 16;
  buffer[4] = packed_size >> 8;
  buffer[5] = packed_size;
  memcpy(buffer + 6, message->payload->data, packed_size);

  return 6 + packed_size;
}

void mumble_message_free(MumbleMessage *message) {
  g_byte_array_unref(message->payload);
  g_free(message);
}

MumbleMessage *mumble_message_copy(MumbleMessage *message) {
  MumbleMessage *copy = mumble_message_new(message->type, message->payload);
  return copy;
}

MumbleMessage *mumble_message_new(MumbleMessageType type, GByteArray *protobuf_message) {
  MumbleMessage *message = g_new0(MumbleMessage, 1);

  message->type = type;
  message->payload = protobuf_message;

  return message;
}
