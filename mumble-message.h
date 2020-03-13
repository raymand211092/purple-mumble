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

#ifndef MUMBLE_MESSAGE_H
#define MUMBLE_MESSAGE_H

#include <glib-object.h>

/**
 * SECTION:mumblemessage
 * @short_description: Mumble protocol message
 *
 * This data structure is used for the serialization and deserialization of
 * Mumble protocol messages. Mumble messages consist of prefix and payload.
 * Prefix is 6 bytes long and includes type of the message and size of the
 * payload. Payload is defined using [Protocol Buffers](https://developers.google.com/protocol-buffers/)
 * and the actual serialization and deserialization is carried out by
 * [protobuf-c](https://github.com/protobuf-c/protobuf-c).
 */

/**
 * MumbleMessageType:
 * @MUMBLE_VERSION:               0
 * @MUMBLE_UDP_TUNNEL:            1
 * @MUMBLE_AUTHENTICATE:          2
 * @MUMBLE_PING:                  3
 * @MUMBLE_REJECT:                4
 * @MUMBLE_SERVER_SYNC:           5
 * @MUMBLE_CHANNEL_REMOVE:        6
 * @MUMBLE_CHANNEL_STATE:         7
 * @MUMBLE_USER_REMOVE:           8
 * @MUMBLE_USER_STATE:            9
 * @MUMBLE_BAN_LIST:              10
 * @MUMBLE_TEXT_MESSAGE:          11
 * @MUMBLE_PERMISSION_DENIED:     12
 * @MUMBLE_ACL:                   13
 * @MUMBLE_QUERY_USERS:           14
 * @MUMBLE_CRYPT_SETUP:           15
 * @MUMBLE_CONTEXT_ACTION_MODIFY: 16
 * @MUMBLE_CONTEXT_ACTION:        17
 * @MUMBLE_USER_LIST:             18
 * @MUMBLE_VOICE_TARGET:          19
 * @MUMBLE_PERMISSION_QUERY:      20
 * @MUMBLE_CODEC_VERSION:         21
 * @MUMBLE_USER_STATS:            22
 * @MUMBLE_REQUEST_BLOB:          23
 * @MUMBLE_SERVER_CONFIG:         24
 * @MUMBLE_SUGGEST_CONFIG:        25
 *
 * Used for defining the type of a #MumbleMessage.
 */
typedef enum {
  MUMBLE_VERSION,
  MUMBLE_UDP_TUNNEL,
  MUMBLE_AUTHENTICATE,
  MUMBLE_PING,
  MUMBLE_REJECT,
  MUMBLE_SERVER_SYNC,
  MUMBLE_CHANNEL_REMOVE,
  MUMBLE_CHANNEL_STATE,
  MUMBLE_USER_REMOVE,
  MUMBLE_USER_STATE,
  MUMBLE_BAN_LIST,
  MUMBLE_TEXT_MESSAGE,
  MUMBLE_PERMISSION_DENIED,
  MUMBLE_ACL,
  MUMBLE_QUERY_USERS,
  MUMBLE_CRYPT_SETUP,
  MUMBLE_CONTEXT_ACTION_MODIFY,
  MUMBLE_CONTEXT_ACTION,
  MUMBLE_USER_LIST,
  MUMBLE_VOICE_TARGET,
  MUMBLE_PERMISSION_QUERY,
  MUMBLE_CODEC_VERSION,
  MUMBLE_USER_STATS,
  MUMBLE_REQUEST_BLOB,
  MUMBLE_SERVER_CONFIG,
  MUMBLE_SUGGEST_CONFIG
} MumbleMessageType;

/**
 * MumbleMessage:
 * @type:    Message type
 * @payload: Message content
 *
 * Represents a Mumble protocol message.
 */
typedef struct _MumbleMessage {
  MumbleMessageType type;
  GByteArray *payload;
} MumbleMessage;

GType mumble_message_get_type(void);

/**
 * mumble_message_read:
 * @buffer: Byte buffer
 * @length: Length of @buffer
 *
 * Deserialize a message from @buffer. Length of the message in @buffer can be
 * determined with mumble_message_get_minimum_length().
 *
 * Returns: New #MumbleMessage
 */
MumbleMessage *mumble_message_read(guint8 *buffer, guint length);

/**
 * mumble_message_get_minimum_length:
 * @buffer: Byte buffer
 * @length: Length of @buffer
 *
 * Given a partial message in @buffer determine the minimum number of bytes
 * required for the complete message.
 *
 * Returns: Integer
 */
guint mumble_message_get_minimum_bytes(guint8 *buffer, guint length);

/**
 * mumble_message_write:
 * @message: A #MumbleMessage to serialize
 * @buffer:  Destination buffer
 *
 * Serialize @message to @buffer.
 *
 * Returns: Length of the serialized message in @buffer
 */
gint mumble_message_write(MumbleMessage *message, guint8 *buffer);

/**
 * mumble_message_free:
 * @message: A #MumbleMessage
 *
 * Free memory allocated for @message.
 */
void mumble_message_free(MumbleMessage *message);

/**
 * mumble_message_copy:
 * @message: A #MumbleMessage
 *
 * Create a new copy of @message.
 *
 * Returns: Copy of @message
 */
MumbleMessage *mumble_message_copy(MumbleMessage *message);

/**
 * mumble_message_new:
 * @type:    Message type
 * @payload: Payload as #GByteArray
 *
 * Create new #MumbleMessage.
 *
 * Returns: New #MumbleMessage
 */
MumbleMessage *mumble_message_new(MumbleMessageType type, GByteArray *payload);

#endif
