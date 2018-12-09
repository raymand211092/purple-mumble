/*
 * purple-mumble -- Mumble protocol plugin for libpurple
 * Copyright (C) 2018  Petteri Pitkänen
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

ProtobufCMessageDescriptor *typeToDescriptor[] = {
  &mumble_proto__version__descriptor,
  &mumble_proto__udptunnel__descriptor,
  &mumble_proto__authenticate__descriptor,
  &mumble_proto__ping__descriptor,
  &mumble_proto__reject__descriptor,
  &mumble_proto__server_sync__descriptor,
  &mumble_proto__channel_remove__descriptor,
  &mumble_proto__channel_state__descriptor,
  &mumble_proto__user_remove__descriptor,
  &mumble_proto__user_state__descriptor,
  &mumble_proto__ban_list__descriptor,
  &mumble_proto__text_message__descriptor,
  &mumble_proto__permission_denied__descriptor,
  &mumble_proto__acl__descriptor,
  &mumble_proto__query_users__descriptor,
  &mumble_proto__crypt_setup__descriptor,
  &mumble_proto__context_action_modify__descriptor,
  &mumble_proto__context_action__descriptor,
  &mumble_proto__user_list__descriptor,
  &mumble_proto__voice_target__descriptor,
  &mumble_proto__permission_query__descriptor,
  &mumble_proto__codec_version__descriptor,
  &mumble_proto__user_stats__descriptor,
  &mumble_proto__request_blob__descriptor,
  &mumble_proto__server_config__descriptor,
  &mumble_proto__suggest_config__descriptor
};

G_DEFINE_BOXED_TYPE(MumbleMessage, mumble_message, mumble_message_copy, mumble_message_free)

MumbleMessage *mumble_message_read(guint8 *buffer, gint length) {
  MumbleMessage *message = NULL;
  if (length >= 6) {
    int type = buffer[1];
    int messageLength = mumble_message_get_minimum_length(buffer, length);

    if (length >= messageLength) {
      message = mumble_message_new(type, protobuf_c_message_unpack(typeToDescriptor[type], NULL, messageLength - 6, buffer + 6));
      message->unpacked = TRUE;
    }
  }
  return message;
}

gint mumble_message_get_minimum_length(guint8 *buffer, gint partialLength) {
  gint minimumLength;
  if (partialLength < 6) {
    minimumLength = 6;
  } else {
    gint payloadLength = (buffer[2] << 24) | (buffer[3] << 16) | (buffer[4] << 8) | buffer[5];
    minimumLength = 6 + payloadLength;
  }
  return minimumLength;
}

gint mumble_message_write(MumbleMessage *message, guint8 *buffer) {
  gint packedSize = protobuf_c_message_get_packed_size(message->payload);

  buffer[0] = 0;
  buffer[1] = message->type;
  buffer[2] = packedSize << 24;
  buffer[3] = packedSize << 16;
  buffer[4] = packedSize << 8;
  buffer[5] = packedSize;

  return 6 + protobuf_c_message_pack(message->payload, buffer + 6);
}

void mumble_message_free(MumbleMessage *message) {
  if (message->unpacked) {
    protobuf_c_message_free_unpacked(message->payload, NULL);
  }
  g_free(message);
}

MumbleMessage *mumble_message_copy(MumbleMessage *message) {
  MumbleMessage *copy = mumble_message_new(message->type, message->payload);
  copy->unpacked = message->unpacked;
  return copy;
}

MumbleMessage *mumble_message_new(MumbleMessageType type, ProtobufCMessage *protobufMessage) {
  MumbleMessage *message = g_new0(MumbleMessage, 1);

  message->type = type;
  message->payload = protobufMessage;

  return message;
}
