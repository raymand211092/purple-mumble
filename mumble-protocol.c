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

#include <glib/gi18n.h>
#include <purple-gio.h>
#include <queuedoutputstream.h>
#include "mumble-protocol.h"
#include "mumble-message.h"

typedef struct {
  guint32 sessionId;
  gchar *name;
} MumbleUser;

typedef struct {
  GSocketConnection *connection;
  PurpleQueuedOutputStream *outputStream;
  GInputStream *inputStream;
  GCancellable *cancellable;
  guint8 *inputBuffer;
  gint inputBufferIndex;
  gchar *userName;
  gchar *server;
  PurpleChatConversation *rootChatConversation;
  GHashTable *idToUser;
  guint keepalive;
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
  protocol->id   = "prpl-mumble";
  protocol->name = "Mumble";
  
  protocol->options = OPT_PROTO_PASSWORD_OPTIONAL;
  
  protocol->user_splits = g_list_append(protocol->user_splits, purple_account_user_split_new(_("Server"), "localhost", '@'));
  
  protocol->account_options = g_list_append(protocol->account_options, purple_account_option_int_new(_("Port"), "port", 64738));
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
  PurpleConnection *connection = purple_account_get_connection(account);
  
  MumbleProtocolData *protocolData = g_new0(MumbleProtocolData, 1);
  purple_connection_set_protocol_data(connection, protocolData);
  
  gchar **parts = g_strsplit(purple_account_get_username(account), "@", 2);
  protocolData->userName = g_strdup(parts[0]);
  protocolData->server = g_strdup(parts[1]);
  g_strfreev(parts);
  
  GError *error;
  GSocketClient *client = purple_gio_socket_client_new(account, &error);
  if (!client) {
    purple_connection_take_error(connection, error);
    return;
  }
  g_socket_client_set_tls(client, TRUE);
  g_socket_client_set_tls_validation_flags(client, 0);
  
  g_socket_client_connect_to_host_async(client, protocolData->server, purple_account_get_int(account, "port", 64738), NULL, on_connected, connection);
  
  g_object_unref(client);
  
  purple_connection_set_state(connection, PURPLE_CONNECTION_CONNECTING);
}

static void mumble_protocol_close(PurpleConnection *connection) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
  if (protocolData->rootChatConversation) {
    purple_serv_got_chat_left(connection, purple_chat_conversation_get_id(protocolData->rootChatConversation));
  }
  purple_account_set_status(purple_connection_get_account(connection), "offline", TRUE, NULL);
  
  g_cancellable_cancel(protocolData->cancellable);
  g_clear_object(&protocolData->cancellable);
  
  g_source_remove(protocolData->keepalive);
  purple_gio_graceful_close(G_IO_STREAM(protocolData->connection), G_INPUT_STREAM(protocolData->inputStream), G_OUTPUT_STREAM(protocolData->outputStream));
  
  g_clear_object(&protocolData->inputStream);
  g_clear_object(&protocolData->outputStream);
  g_clear_object(&protocolData->connection);
  
  g_hash_table_destroy(protocolData->idToUser);
  
  g_free(protocolData->inputBuffer);
  g_free(protocolData->userName);
  g_free(protocolData->server);
  g_free(protocolData);
}

static GList *mumble_protocol_status_types(PurpleAccount *account) {
  GList *types = NULL;
  types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_OFFLINE, NULL, NULL, TRUE));
  types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_AVAILABLE, NULL, NULL, TRUE));
  return types;
}

static const char *mumble_protocol_list_icon(PurpleAccount *account, PurpleBuddy *buddy) {
  return "mumble";
}

static gssize mumble_protocol_client_iface_get_max_message_size(PurpleConversation *conversation) {
  return 256;
}

static GList *mumble_protocol_chat_iface_info(PurpleConnection *connection) {
  GList *info = NULL;
  PurpleProtocolChatEntry *entry;
  
  entry = g_new0(PurpleProtocolChatEntry, 1);
  entry->label = "Channel:";
  entry->identifier = "channel";
  entry->required = TRUE;
  info = g_list_append(info, entry);
  
  return info;
}

static GHashTable *mumble_protocol_chat_iface_info_defaults(PurpleConnection *connection, const char *chatName) {
  GHashTable *defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  
  if (chatName) {
    g_hash_table_insert(defaults, "channel", g_strdup(chatName));
  }
  
  return defaults;
}

static void mumble_protocol_chat_iface_join(PurpleConnection *connection, GHashTable *components) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
  protocolData->rootChatConversation = purple_serv_got_joined_chat(connection, 0, "root");
  purple_chat_conversation_add_user(protocolData->rootChatConversation, protocolData->userName, NULL, 0, FALSE);
}

static void mumble_protocol_chat_iface_leave(PurpleConnection *connection, int id) {
  purple_serv_got_chat_left(connection, id);
}

static int mumble_protocol_chat_iface_send(PurpleConnection *connection, int id, PurpleMessage *message) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
  MumbleProto__TextMessage textMessage = MUMBLE_PROTO__TEXT_MESSAGE__INIT;
  int ids[1] = { 0 };
  textMessage.has_actor = 0;
  textMessage.n_channel_id = 1;
  textMessage.channel_id = ids;
  textMessage.message = purple_message_get_contents(message);
  write_mumble_message(protocolData, MUMBLE_TEXT_MESSAGE, (ProtobufCMessage *) &textMessage);
  
  purple_serv_got_chat_in(connection, 0, protocolData->userName, purple_message_get_flags(message), purple_message_get_contents(message), time(NULL));
  
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
  PurpleConnection *purpleConnection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(purpleConnection);
  
  GError *error = NULL;
  protocolData->connection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, &error);
  if (error) {
    purple_connection_take_error(purpleConnection, error);
    return;
  }
  
  protocolData->idToUser = g_hash_table_new(g_int_hash, g_int_equal);
  
  protocolData->outputStream = purple_queued_output_stream_new(g_io_stream_get_output_stream(G_IO_STREAM(protocolData->connection)));
  
  protocolData->inputStream = g_io_stream_get_input_stream(G_IO_STREAM(protocolData->connection));
  protocolData->inputBuffer = g_malloc(256 * 1024);
  protocolData->inputBufferIndex = 0;
  
  protocolData->cancellable = g_cancellable_new();
  
  read_asynchronously(purpleConnection, 6);
  
  MumbleProto__Version versionMessage = MUMBLE_PROTO__VERSION__INIT;
  versionMessage.has_version = 1;
  versionMessage.version = 0x010213;
  versionMessage.release = "foo";
  versionMessage.os = "bar";
  versionMessage.os_version = "baz";
  write_mumble_message(protocolData, MUMBLE_VERSION, (ProtobufCMessage *) &versionMessage);
  
  MumbleProto__Authenticate authenticateMessage = MUMBLE_PROTO__AUTHENTICATE__INIT;
  authenticateMessage.username = protocolData->userName;
  write_mumble_message(protocolData, MUMBLE_AUTHENTICATE, (ProtobufCMessage *) &authenticateMessage);
  
  MumbleProto__Ping pingMessage = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocolData, MUMBLE_PING, (ProtobufCMessage *) &pingMessage);
  
  protocolData->keepalive = g_timeout_add_seconds(10, on_keepalive, purpleConnection);
  
  purple_connection_set_state(purpleConnection, PURPLE_CONNECTION_CONNECTED);
  
  protocolData->rootChatConversation = purple_serv_got_joined_chat(purpleConnection, 0, "root");
  purple_chat_conversation_add_user(protocolData->rootChatConversation, protocolData->userName, NULL, 0, FALSE);
}

static void read_asynchronously(PurpleConnection *connection, gint count) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  g_input_stream_read_async(protocolData->inputStream, protocolData->inputBuffer + protocolData->inputBufferIndex, count, G_PRIORITY_DEFAULT, protocolData->cancellable, on_read, connection);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);
  
  GError *error = NULL;
  gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
  if (error) {
    purple_connection_take_error(connection, error);
    return;
  } else if (count <= 0) {
    purple_connection_take_error(connection, g_error_new_literal(PURPLE_CONNECTION_ERROR, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, _("Server closed the connection")));
    return;
  }
  
  protocolData->inputBufferIndex += count;
  
  MumbleMessage *message = mumble_message_read(protocolData->inputBuffer, protocolData->inputBufferIndex);
  int nextCount;
  
  if (message) {
    switch (message->type) {
      case MUMBLE_VERSION: {
        MumbleProto__Version *version = (MumbleProto__Version *) message->payload;
        purple_debug_info("mumble", "MUMBLE_VERSION: version=%d release=%s os=%s os_version=%s", version->version, version->release, version->os, version->os_version);
        break;
      }
      case MUMBLE_PING: {
        MumbleProto__Ping *ping = (MumbleProto__Ping *) message->payload;
        purple_debug_info("mumble", "MUMBLE_PING: timestamp=%d good=%d late=%d lost=%d resync=%d udp_packets=%d tcp_packets=%d upd_ping_avg=%f udp_ping_var=%f tcp_ping_avg=%f tcp_ping_var=%f", ping->timestamp, ping->good, ping->late, ping->lost, ping->resync,
          ping->udp_packets, ping->tcp_packets, ping->udp_ping_avg, ping->udp_ping_var, ping->tcp_ping_avg, ping->tcp_ping_var);
        break;
      }
      case MUMBLE_REJECT: {
        MumbleProto__Reject *reject = (MumbleProto__Reject *) message->payload;
        purple_debug_info("mumble", "MUMBLE_REJECT: type=%d reason=%s", reject->type, reject->reason);
        break;
      }
      case MUMBLE_SERVER_SYNC: {
        MumbleProto__ServerSync *serverSync = (MumbleProto__ServerSync *) message->payload;
        purple_debug_info("mumble", "MUMBLE_SERVER_SYNC: session=%d max_badwidth=%d welcome_text=%s permissions=%d", serverSync->session, serverSync->max_bandwidth, serverSync->welcome_text, serverSync->permissions);
        break;
      }
      case MUMBLE_CHANNEL_REMOVE: {
        MumbleProto__ChannelRemove *channelRemove = (MumbleProto__ChannelRemove *) message->payload;
        purple_debug_info("mumble", "MUMBLE_CHANNEL_REMOVE: channel_id=%d", channelRemove->channel_id);
        break;
      }
      case MUMBLE_CHANNEL_STATE: {
        MumbleProto__ChannelState *channelState = (MumbleProto__ChannelState *) message->payload;
        purple_debug_info("mumble", "MUMBLE_CHANNEL_STATE: channel_id=%d parent=%d name=%s description=%s temporary=%d position=%d max_users=%d", channelState->channel_id, channelState->parent, channelState->name, channelState->description,
          channelState->has_temporary ? channelState->temporary : -1, channelState->has_position ? channelState->position : -1, channelState->has_max_users ? channelState->max_users : -1);
        purple_debug_info("mumble", "Links:");
        for (int index = 0; index < channelState->n_links; index++) {
          purple_debug_info("mumble", "%d", channelState->links[index]);
        }
        purple_debug_info("mumble", "Links to add:");
        for (int index = 0; index < channelState->n_links_add; index++) {
          purple_debug_info("mumble", "%d", channelState->links_add[index]);
        }
        purple_debug_info("mumble", "Links to remove:");
        for (int index = 0; index < channelState->n_links_remove; index++) {
          purple_debug_info("mumble", "%d", channelState->links_remove[index]);
        }
        break;
      }
      case MUMBLE_USER_REMOVE: {
        MumbleProto__UserRemove *userRemove = (MumbleProto__UserRemove *) message->payload;

        purple_debug_info("mumble", "MUMBLE_USER_REMOVE: session=%d actor=%d reason=%s ban=%d", userRemove->session, userRemove->actor, userRemove->reason, userRemove->ban);

        MumbleUser *user = g_hash_table_lookup(protocolData->idToUser, &userRemove->session);
        if (user) {
          purple_chat_conversation_remove_user(protocolData->rootChatConversation, user->name, NULL);

          g_hash_table_remove(protocolData->idToUser, &user->sessionId);
          g_free(user->name);
          g_free(user);
        }
        break;
      }
      case MUMBLE_USER_STATE: {
        MumbleProto__UserState *userState = (MumbleProto__UserState *) message->payload;

        purple_debug_info("mumble", "MUMBLE_USER_STATE: session=%d name=%s channel_id=%d mute=%d deaf=%d suppress=%d self_mute=%d self_deaf=%d has_texture=%d comment=%s hash=%s has_comment_hash=%d has_texture_hash=%d priority_speaker=%d recording=%d",
          userState->session, userState->name, userState->channel_id, userState->mute, userState->deaf, userState->suppress, userState->self_mute, userState->self_deaf, userState->has_texture, userState->comment, userState->hash, userState->has_comment_hash,
          userState->has_texture_hash, userState->priority_speaker, userState->recording);

        guint32 userId = userState->session;

        if (!g_hash_table_contains(protocolData->idToUser, &userId)) {
          MumbleUser *user = g_new(MumbleUser, 1);
          user->sessionId = userId;
          user->name = g_strdup(userState->name);

          g_hash_table_insert(protocolData->idToUser, &user->sessionId, user);

          if (!purple_chat_conversation_find_user(protocolData->rootChatConversation, userState->name)) {
            purple_chat_conversation_add_user(protocolData->rootChatConversation, userState->name, NULL, 0, FALSE);
          }
        }
        break;
      }
      case MUMBLE_BAN_LIST: {
        MumbleProto__BanList *banList = (MumbleProto__BanList *) message->payload;

        purple_debug_info("mumble", "MUMBLE_BAN_LIST: query=%d", banList->query);
        for (int index = 0; index < banList->n_bans; index++) {
          MumbleProto__BanList__BanEntry *entry = banList->bans[index];
          purple_debug_info("mumble", "address= mask=%d name=%s hash=%s reason=%s start=%s duration=%d", entry->mask, entry->mask, entry->hash, entry->reason, entry->start, entry->duration);
        }
        break;
      }
      case MUMBLE_TEXT_MESSAGE: {
        MumbleProto__TextMessage *textMessage = (MumbleProto__TextMessage *) message->payload;

        purple_debug_info("mumble", "MUMBLE_TEXT_MESSAGE: actor=%d message=%s", textMessage->has_actor ? textMessage->actor : -1, textMessage->message);

        MumbleUser *sender = g_hash_table_lookup(protocolData->idToUser, &textMessage->actor);

        purple_serv_got_chat_in(connection, 0, sender->name, 0, textMessage->message, time(NULL));
        break;
      }
      case MUMBLE_PERMISSION_DENIED: {
        MumbleProto__PermissionDenied *permissionDenied = (MumbleProto__PermissionDenied *) message->payload;
        purple_debug_info("mumble", "MUMBLE_PERMISSION_DENIED: permission=%d channel_id=%d session=%d reason=%s type=%d name=%s", permissionDenied->permission, permissionDenied->channel_id, permissionDenied->session, permissionDenied->reason,
          permissionDenied->type, permissionDenied->name);
        break;
      }
      case MUMBLE_CRYPT_SETUP: {
        break;
      }
      case MUMBLE_ACL: {
        MumbleProto__ACL *acl = (MumbleProto__ACL *) message->payload;
        purple_debug_info("mumble", "MUMBLE_ACL: channel_id=%d inherit_acls=%d query=%d", acl->inherit_acls, acl->query);
        purple_debug_info("mumble", "User group specifications:");
        for (int index = 0; index < acl->n_groups; index++) {
          MumbleProto__ACL__ChanGroup *group = acl->groups[index];
          purple_debug_info("mumble", "name=%s inherited=%d inherit=%d inheritable=%d");
          purple_debug_info("mumble", "Users explicitly included in this group:");
          for (int userIndex = 0; userIndex < group->n_add; userIndex++) {
            purple_debug_info("mumble", "%d", group->add[userIndex]);
          }
          purple_debug_info("mumble", "Users explicitly removed from this group:");
          for (int userIndex = 0; userIndex < group->n_remove; userIndex++) {
            purple_debug_info("mumble", "%d", group->remove[userIndex]);
          }
          purple_debug_info("mumble", "Users inherited:");
          for (int userIndex = 0; userIndex < group->n_inherited_members; userIndex++) {
            purple_debug_info("mumble", "%d", group->inherited_members[userIndex]);
          }
        }
        purple_debug_info("mumble", "ACL specifications");
        for (int index = 0; index < acl->n_acls; index++) {
          MumbleProto__ACL__ChanACL *channelAcl = acl->acls[index];
          purple_debug_info("mumble", "apply_here=%d apply_subs=%d inherited=%d user_id=%d group=%s grant=%d deny=%d", channelAcl->apply_here, channelAcl->apply_subs, channelAcl->inherited, channelAcl->user_id, channelAcl->group, channelAcl->grant, channelAcl->deny);
        }
        break;
      }
      case MUMBLE_PERMISSION_QUERY: {
        MumbleProto__PermissionQuery *permissionQuery = (MumbleProto__PermissionQuery *) message->payload;
        purple_debug_info("mumble", "MUMBLE_PERMISSION_QUERY: channel_id=%d permissions=%d flush=%d", permissionQuery->channel_id, permissionQuery->permissions, permissionQuery->flush);
        break;
      }
      case MUMBLE_CODEC_VERSION: {
        MumbleProto__CodecVersion *codecVersion = (MumbleProto__CodecVersion *) message->payload;
        purple_debug_info("mumble", "MUMBLE_CODEC_VERSION: alpha=%d beta=%d prefer_alpha=%d opus=%d", codecVersion->alpha, codecVersion->beta, codecVersion->prefer_alpha, codecVersion->opus);
        break;
      }
      case MUMBLE_USER_STATS: {
        MumbleProto__UserStats *userStats = (MumbleProto__UserStats *) message->payload;
        purple_debug_info("mumble", "MUMBLE_USER_STATS: session=%d stats_only=%d n_certificates=%d udp_packets=%d tcp_packets=%d udp_ping_avg=%f udp_ping_var=%f tcp_ping_avg=%f tcp_ping_var=%f n_celt_versions=%d address= bandwidth=%d onlinesecs=%d idlesecs=%d strong_certificate=%d opus=%d",
          userStats->session, userStats->stats_only, userStats->udp_packets, userStats->tcp_packets, userStats->udp_ping_avg, userStats->udp_ping_var, userStats->tcp_ping_avg, userStats->tcp_ping_var, userStats->n_celt_versions, userStats->bandwidth,
          userStats->onlinesecs, userStats->idlesecs, userStats->strong_certificate, userStats->opus);
        purple_debug_info("mumble", "Statistics for packets received from client: good=%d late=%d lost=%d resync=%d", userStats->from_client->good, userStats->from_client->late, userStats->from_client->lost, userStats->from_client->resync);
        purple_debug_info("mumble", "Statistics for packets sent by server: good=%d late=%d lost=%d resync=%d", userStats->from_server->good, userStats->from_server->late, userStats->from_server->lost, userStats->from_server->resync);
        break;
      }
      case MUMBLE_SERVER_CONFIG: {
        MumbleProto__ServerConfig *serverConfig = (MumbleProto__ServerConfig *) message->payload;
        purple_debug_info("mumble", "MUMBLE_SERVER_CONFIG: max_bandwidth=%d welcome_text=%s allow_html=%d message_length=%d image_message_length=%d max_users=%d", serverConfig->max_bandwidth, serverConfig->welcome_text, serverConfig->allow_html,
          serverConfig->message_length, serverConfig->image_message_length, serverConfig->max_users);
        break;
      }
      case MUMBLE_SUGGEST_CONFIG: {
        MumbleProto__SuggestConfig *suggestConfig = (MumbleProto__SuggestConfig *) message->payload;
        purple_debug_info("mumble", "MUMBLE_SUGGEST_CONFIG: version=%d positional=%d push_to_talk=%d", suggestConfig->version, suggestConfig->positional, suggestConfig->push_to_talk);
        break;
      }
      default: {
        purple_debug_info("mumble", "Read message of type %d", message->type);
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
  purple_queued_output_stream_push_bytes_async(protocolData->outputStream, bytes, G_PRIORITY_DEFAULT, protocolData->cancellable, on_mumble_message_written, bytes);
  
  mumble_message_free(message);
}

static void on_mumble_message_written(GObject *source, GAsyncResult *result, gpointer data) {
  GBytes *bytes = data;
  g_bytes_unref(bytes);
}
