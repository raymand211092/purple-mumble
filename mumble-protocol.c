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

#include <purple-gio.h>
#include <queuedoutputstream.h>
#include "mumble-protocol.h"
#include "mumble-message.h"

struct mumble_protocol_data {
  PurpleQueuedOutputStream *outputStream;
  GInputStream *inputStream;
  guint8 *inputBuffer;
  gint inputBufferIndex;
};

static void mumble_protocol_init(PurpleProtocol *);
static void mumble_protocol_class_init(PurpleProtocolClass *);

static void mumble_protocol_login(PurpleAccount *);
static void mumble_protocol_close(PurpleConnection *);
static GList *mumble_protocol_status_types(PurpleAccount *);
static const char *mumble_protocol_list_icon(PurpleAccount *, PurpleBuddy *);

PURPLE_DEFINE_TYPE(MumbleProtocol, mumble_protocol, PURPLE_TYPE_PROTOCOL);

G_MODULE_EXPORT GType mumble_protocol_get_type(void);

static gboolean on_keepalive(gpointer);
static void on_connected(GObject *, GAsyncResult *, gpointer);
static void read_asynchronously(PurpleConnection *, gint);
static void on_read(GObject *, GAsyncResult *, gpointer);
static void write_mumble_message(struct mumble_protocol_data *, MumbleMessageType, ProtobufCMessage *);
static void on_mumble_message_written(GObject *, GAsyncResult *, gpointer);

static void mumble_protocol_init(PurpleProtocol *protocol) {
  fprintf(stderr, "mumble_protocol_init()\n");
  
  protocol->id   = "prpl-mumble";
  protocol->name = "Mumble";
  protocol->options = OPT_PROTO_PASSWORD_OPTIONAL;
}

static void mumble_protocol_class_init(PurpleProtocolClass *protocolClass) {
  protocolClass->login        = mumble_protocol_login;
  protocolClass->close        = mumble_protocol_close;
  protocolClass->status_types = mumble_protocol_status_types;
  protocolClass->list_icon    = mumble_protocol_list_icon;
}

static void mumble_protocol_login(PurpleAccount *account) {
  fprintf(stderr, "mumble_protocol_login()\n");
  
  PurpleConnection *connection = purple_account_get_connection(account);
  
  struct mumble_protocol_data *protocol_data = g_new0(struct mumble_protocol_data, 1);
  purple_connection_set_protocol_data(connection, protocol_data);
  
  GError *error;
  GSocketClient *client = purple_gio_socket_client_new(account, &error);
  if (!client) {
    purple_connection_take_error(connection, error);
    return;
  }
  g_socket_client_set_tls(client, TRUE);
  g_socket_client_set_tls_validation_flags(client, 0);
  
  g_socket_client_connect_to_host_async(client, "127.0.0.1:64738", 64738, NULL, on_connected, connection);
  
  g_object_unref(client);
}

static void mumble_protocol_close(PurpleConnection *connection) {
  fprintf(stderr, "mumble_protocol_close()\n");
}

static GList *mumble_protocol_status_types(PurpleAccount *account) {
  fprintf(stderr, "mumble_protocol_status_types()\n");
  GList *types = NULL;
  types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_AVAILABLE, NULL, NULL, FALSE));
  return types;
}

static const char *mumble_protocol_list_icon(PurpleAccount *account, PurpleBuddy *buddy) {
  fprintf(stderr, "mumble_protocol_list_icon()\n");
  return "mumble";
}

static gboolean on_keepalive(gpointer data) {
  PurpleConnection *connection = data;
  struct mumble_protocol_data *protocolData = purple_connection_get_protocol_data(connection);
  
  MumbleProto__Ping pingMessage = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocolData, MUMBLE_PING, (ProtobufCMessage *) &pingMessage);
  
  return TRUE;
}

static void on_connected(GObject *source, GAsyncResult *result, gpointer data) {
  fprintf(stderr, "on_connected()\n");

  PurpleConnection *purpleConnection = data;
  struct mumble_protocol_data *protocolData = purple_connection_get_protocol_data(purpleConnection);
  
  GError *error = NULL;
  GSocketConnection *socketConnection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, &error);
  if (error) {
    purple_connection_take_error(purpleConnection, error);
    return;
  }
  
  protocolData->outputStream = purple_queued_output_stream_new(g_io_stream_get_output_stream(G_IO_STREAM(socketConnection)));
  
  protocolData->inputStream = g_io_stream_get_input_stream(G_IO_STREAM(socketConnection));
  protocolData->inputBuffer = g_malloc(64 * 1024);
  protocolData->inputBufferIndex = 0;
  
  read_asynchronously(purpleConnection, 6);
  
  MumbleProto__Version versionMessage = MUMBLE_PROTO__VERSION__INIT;
  versionMessage.has_version = 1;
  versionMessage.version = 0x010213;
  versionMessage.release = "foo";
  versionMessage.os = "bar";
  versionMessage.os_version = "baz";
  write_mumble_message(protocolData, MUMBLE_VERSION, (ProtobufCMessage *) &versionMessage);
  
  MumbleProto__Authenticate authenticateMessage = MUMBLE_PROTO__AUTHENTICATE__INIT;
  authenticateMessage.username = "username";
  write_mumble_message(protocolData, MUMBLE_AUTHENTICATE, (ProtobufCMessage *) &authenticateMessage);
  
  MumbleProto__Ping pingMessage = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocolData, MUMBLE_PING, (ProtobufCMessage *) &pingMessage);
  
  g_timeout_add_seconds(10, on_keepalive, purpleConnection);
}

static void read_asynchronously(PurpleConnection *connection, gint count) {
  struct mumble_protocol_data *protocolData = purple_connection_get_protocol_data(connection);
  g_input_stream_read_async(protocolData->inputStream, protocolData->inputBuffer + protocolData->inputBufferIndex, count, G_PRIORITY_DEFAULT, NULL, on_read, connection);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  struct mumble_protocol_data *protocolData = purple_connection_get_protocol_data(connection);
  
  GError *error = NULL;
  gssize count = g_input_stream_read_finish(protocolData->inputStream, result, &error);
  if (error) {
    purple_connection_take_error(connection, error);
    return;
  }
  
  protocolData->inputBufferIndex += count;
  
  MumbleMessage *message = mumble_message_read(protocolData->inputBuffer, protocolData->inputBufferIndex);
  int nextCount;
  
  if (message) {
    fprintf(stderr, "on_read(): %d\n", message->type);
    
    switch (message->type) {
      case MUMBLE_VERSION: {
        MumbleProto__Version *version = (MumbleProto__Version *) message->payload;
        fprintf(stderr, "MUMBLE_VERSION: %s %s %s\n", version->release, version->os, version->os_version);
        break;
      }
      case MUMBLE_UDP_TUNNEL: {
        break;
      }
      case MUMBLE_AUTHENTICATE: {
        break;
      }
      case MUMBLE_PING: {
        MumbleProto__Ping *ping = (MumbleProto__Ping *) message->payload;
        fprintf(stderr, "MUMBLE_PING\n");
        break;
      }
      case MUMBLE_REJECT: {
        break;
      }
      case MUMBLE_SERVER_SYNC: {
        MumbleProto__ServerSync *serverSync = (MumbleProto__ServerSync *) message->payload;
        fprintf(stderr, "MUMBLE_SERVER_SYNC: %s\n", serverSync->welcome_text);
        break;
      }
      case MUMBLE_CHANNEL_REMOVE: {
        break;
      }
      case MUMBLE_CHANNEL_STATE: {
        MumbleProto__ChannelState *channelState = (MumbleProto__ChannelState *) message->payload;
        fprintf(stderr, "MUMBLE_CHANNEL_STATE: %s\n", channelState->name);
        break;
      }
      case MUMBLE_USER_REMOVE: {
        break;
      }
      case MUMBLE_USER_STATE: {
        MumbleProto__UserState *userState = (MumbleProto__UserState *) message->payload;
        fprintf(stderr, "MUMBLE_USER_STATE: %s\n", userState->name);
        break;
      }
      case MUMBLE_BAN_LIST: {
        break;
      }
      case MUMBLE_TEXT_MESSAGE: {
        MumbleProto__TextMessage *textMessage = (MumbleProto__TextMessage *) message->payload;
        fprintf(stderr, "MUMBLE_TEXT_MESSAGE: %d: %s\n", textMessage->has_actor ? textMessage->actor : -1, textMessage->message);
        break;
      }
      case MUMBLE_PERMISSION_DENIED: {
        break;
      }
      case MUMBLE_ACL: {
        break;
      }
      case MUMBLE_QUERY_USERS: {
        break;
      }
      case MUMBLE_CRYPT_SETUP: {
        break;
      }
      case MUMBLE_CONTEXT_ACTION_MODIFY: {
        break;
      }
      case MUMBLE_CONTEXT_ACTION: {
        break;
      }
      case MUMBLE_USER_LIST: {
        break;
      }
      case MUMBLE_VOICE_TARGET: {
        break;
      }
      case MUMBLE_PERMISSION_QUERY: {
        break;
      }
      case MUMBLE_CODEC_VERSION: {
        break;
      }
      case MUMBLE_USER_STATS: {
        break;
      }
      case MUMBLE_REQUEST_BLOB: {
        break;
      }
      case MUMBLE_SERVER_CONFIG: {
        break;
      }
      case MUMBLE_SUGGEST_CONFIG: {
        break;
      }
    }
    
    mumble_message_free(message);
    
    protocolData->inputBufferIndex = 0;
    nextCount = 6;
  } else {
    nextCount = mumble_message_get_minimum_length(protocolData->inputBuffer, protocolData->inputBufferIndex) - protocolData->inputBufferIndex;
  }
  
  read_asynchronously(connection, nextCount);
}

static void write_mumble_message(struct mumble_protocol_data *protocolData, MumbleMessageType type, ProtobufCMessage *payload) {
  MumbleMessage *message = mumble_message_new(type, payload);
  
  guint8 *buffer = g_malloc(64 * 1024);
  gint count = mumble_message_write(message, buffer);
  GBytes *bytes = g_bytes_new_take(buffer, count);
  purple_queued_output_stream_push_bytes_async(protocolData->outputStream, bytes, G_PRIORITY_DEFAULT, NULL, on_mumble_message_written, bytes);
  
  mumble_message_free(message);
}

static void on_mumble_message_written(GObject *source, GAsyncResult *result, gpointer data) {
  GBytes *bytes = data;
  g_bytes_unref(bytes);
}
