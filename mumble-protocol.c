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

typedef struct {
  guint32 sessionId;
  gchar *name;
} MumbleUser;

typedef struct {
  PurpleQueuedOutputStream *outputStream;
  GInputStream *inputStream;
  guint8 *inputBuffer;
  gint inputBufferIndex;
  PurpleChatConversation *rootChatConversation;
  GList *users;
} MumbleProtocolData;

static void mumble_protocol_init(PurpleProtocol *);
static void mumble_protocol_class_init(PurpleProtocolClass *);

static void mumble_protocol_client_iface_init(PurpleProtocolClientIface *);
static void mumble_protocol_server_iface_init(PurpleProtocolServerIface *);
static void mumble_protocol_chat_iface_init(PurpleProtocolChatIface *);

static void mumble_protocol_login(PurpleAccount *);
static void mumble_protocol_close(PurpleConnection *);
static GList *mumble_protocol_status_types(PurpleAccount *);
static const char *mumble_protocol_list_icon(PurpleAccount *, PurpleBuddy *);

static gssize mumble_protocol_client_iface_get_max_message_size(PurpleConversation *);

static GList *mumble_protocol_chat_iface_info(PurpleConnection *);
static GHashTable *mumble_protocol_chat_iface_info_defaults(PurpleConnection *, const char *);
static void mumble_protocol_chat_iface_join(PurpleConnection *, GHashTable *);
static void mumble_protocol_chat_iface_leave(PurpleConnection *, int);
static int mumble_protocol_chat_iface_send(PurpleConnection *, int, PurpleMessage *);

PURPLE_DEFINE_TYPE_EXTENDED(MumbleProtocol, mumble_protocol, PURPLE_TYPE_PROTOCOL, 0,
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_CLIENT_IFACE, mumble_protocol_client_iface_init)
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_SERVER_IFACE, mumble_protocol_server_iface_init)
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_CHAT_IFACE, mumble_protocol_chat_iface_init));

G_MODULE_EXPORT GType mumble_protocol_get_type(void);

static gboolean on_keepalive(gpointer);
static void on_connected(GObject *, GAsyncResult *, gpointer);
static void read_asynchronously(PurpleConnection *, gint);
static void on_read(GObject *, GAsyncResult *, gpointer);
static void write_mumble_message(MumbleProtocolData *, MumbleMessageType, ProtobufCMessage *);
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

static void mumble_protocol_client_iface_init(PurpleProtocolClientIface *iface) {
  iface->get_max_message_size = mumble_protocol_client_iface_get_max_message_size;
}

static void mumble_protocol_server_iface_init(PurpleProtocolServerIface *iface) {
}

static void mumble_protocol_chat_iface_init(PurpleProtocolChatIface *iface) {
  iface->info          = mumble_protocol_chat_iface_info;
  iface->info_defaults = mumble_protocol_chat_iface_info_defaults;
  iface->join          = mumble_protocol_chat_iface_join;
  iface->leave         = mumble_protocol_chat_iface_leave;
  iface->send          = mumble_protocol_chat_iface_send;
}

static void mumble_protocol_login(PurpleAccount *account) {
  fprintf(stderr, "mumble_protocol_login()\n");
  
  PurpleConnection *connection = purple_account_get_connection(account);
  
  MumbleProtocolData *protocolData = g_new0(MumbleProtocolData, 1);
  purple_connection_set_protocol_data(connection, protocolData);
  
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

static gssize mumble_protocol_client_iface_get_max_message_size(PurpleConversation *conversation) {
  return 256;
}

static GList *mumble_protocol_chat_iface_info(PurpleConnection *connection) {
  fprintf(stderr, "mumble_protocol_chat_iface_info()\n");
  return NULL;
}

static GHashTable *mumble_protocol_chat_iface_info_defaults(PurpleConnection *connection, const char *chatName) {
  fprintf(stderr, "mumble_protocol_chat_iface_info_defaults()\n");
  return NULL;
}

static void mumble_protocol_chat_iface_join(PurpleConnection *connection, GHashTable *components) {
  fprintf(stderr, "mumble_protocol_chat_iface_join()\n");
}

static void mumble_protocol_chat_iface_leave(PurpleConnection *connection, int fd) {
  fprintf(stderr, "mumble_protocol_chat_iface_leave()\n");
}

static int mumble_protocol_chat_iface_send(PurpleConnection *connection, int id, PurpleMessage *message) {
  fprintf(stderr, "mumble_protocol_chat_iface_send()\n");
  
  MumbleProto__TextMessage textMessage = MUMBLE_PROTO__TEXT_MESSAGE__INIT;
  int ids[1] = { 0 };
  textMessage.has_actor = 0;
  textMessage.n_channel_id = 1;
  textMessage.channel_id = ids;
  textMessage.message = purple_message_get_contents(message);
  write_mumble_message(purple_connection_get_protocol_data(connection), MUMBLE_TEXT_MESSAGE, (ProtobufCMessage *) &textMessage);
  
  purple_serv_got_chat_in(connection, 0, "username", purple_message_get_flags(message), purple_message_get_contents(message), time(NULL));
  
  return 0;
}

static gboolean on_keepalive(gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
  MumbleProto__Ping pingMessage = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocolData, MUMBLE_PING, (ProtobufCMessage *) &pingMessage);
  
  return TRUE;
}

static void on_connected(GObject *source, GAsyncResult *result, gpointer data) {
  fprintf(stderr, "on_connected()\n");

  PurpleConnection *purpleConnection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(purpleConnection);
  
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
  
  purple_connection_set_state(purpleConnection, PURPLE_CONNECTION_CONNECTED);
  
  protocolData->rootChatConversation = purple_serv_got_joined_chat(purpleConnection, 0, "root");
  purple_chat_conversation_add_user(protocolData->rootChatConversation, "username", NULL, 0, FALSE);
}

static void read_asynchronously(PurpleConnection *connection, gint count) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  g_input_stream_read_async(protocolData->inputStream, protocolData->inputBuffer + protocolData->inputBufferIndex, count, G_PRIORITY_DEFAULT, NULL, on_read, connection);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
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
        
        MumbleUser *user = g_new(MumbleUser, 1);
        user->sessionId = userState->session;
        user->name = g_strdup(userState->name);
        protocolData->users = g_list_append(protocolData->users, user);
        
        if (!purple_chat_conversation_find_user(protocolData->rootChatConversation, userState->name)) {
          purple_chat_conversation_add_user(protocolData->rootChatConversation, userState->name, NULL, 0, FALSE);
        }
        break;
      }
      case MUMBLE_BAN_LIST: {
        break;
      }
      case MUMBLE_TEXT_MESSAGE: {
        MumbleProto__TextMessage *textMessage = (MumbleProto__TextMessage *) message->payload;
        fprintf(stderr, "MUMBLE_TEXT_MESSAGE: %d: %s\n", textMessage->has_actor ? textMessage->actor : -1, textMessage->message);
        
        MumbleUser *sender;
        for (GList *node = protocolData->users; node != NULL; node = node->next) {
          MumbleUser *user = node->data;
          if (textMessage->actor == user->sessionId) {
            sender = user;
            break;
          }
        }
        
        purple_serv_got_chat_in(connection, 0, sender->name, 0, textMessage->message, time(NULL));
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

static void write_mumble_message(MumbleProtocolData *protocolData, MumbleMessageType type, ProtobufCMessage *payload) {
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
