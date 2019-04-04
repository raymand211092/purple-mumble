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
#include <purple.h>
#include "mumble-input-stream.h"
#include "mumble-output-stream.h"
#include "mumble-protocol.h"
#include "mumble-message.h"
#include "mumble-channel-tree.h"
#include "utils.h"
#include "plugin.h"

typedef struct {
  GSocketConnection *connection;
  MumbleInputStream *inputStream;
  MumbleOutputStream *outputStream;
  GCancellable *cancellable;
  gchar *userName;
  gchar *server;
  GList *registeredCmds;
  PurpleChatConversation *activeChat;
  MumbleChannelTree *tree;
  guint sessionId;
} MumbleProtocolData;

static void mumble_protocol_init(PurpleProtocol *);
static void mumble_protocol_class_init(PurpleProtocolClass *);

static void mumble_protocol_client_iface_init(PurpleProtocolClientIface *);
static void mumble_protocol_server_iface_init(PurpleProtocolServerIface *);
static void mumble_protocol_chat_iface_init(PurpleProtocolChatIface *);
static void mumble_protocol_roomlist_iface_init(PurpleProtocolRoomlistIface *);

static void mumble_protocol_login(PurpleAccount *);
static void mumble_protocol_close(PurpleConnection *);
static GList *mumble_protocol_status_types(PurpleAccount *);
static const char *mumble_protocol_list_icon(PurpleAccount *, PurpleBuddy *);

static gssize mumble_protocol_client_iface_get_max_message_size(PurpleConversation *);

static void mumble_protocol_server_iface_keepalive(PurpleConnection *connection);
static int mumble_protocol_server_iface_get_keepalive_interval();

static GList *mumble_protocol_chat_iface_info(PurpleConnection *);
static GHashTable *mumble_protocol_chat_iface_info_defaults(PurpleConnection *, const char *);
static void mumble_protocol_chat_iface_join(PurpleConnection *, GHashTable *);
static void mumble_protocol_chat_iface_leave(PurpleConnection *, int);
static int mumble_protocol_chat_iface_send(PurpleConnection *, int, PurpleMessage *);

static PurpleRoomlist *mumble_protocol_roomlist_iface_get_list(PurpleConnection *);

PURPLE_DEFINE_TYPE_EXTENDED(MumbleProtocol, mumble_protocol, PURPLE_TYPE_PROTOCOL, 0,
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_CLIENT_IFACE, mumble_protocol_client_iface_init)
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_SERVER_IFACE, mumble_protocol_server_iface_init)
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_CHAT_IFACE, mumble_protocol_chat_iface_init)
  PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_ROOMLIST_IFACE, mumble_protocol_roomlist_iface_init));

G_MODULE_EXPORT GType mumble_protocol_get_type(void);

static void on_connected(GObject *, GAsyncResult *, gpointer);
static void on_read(GObject *, GAsyncResult *, gpointer);
static void write_mumble_message(MumbleProtocolData *, MumbleMessageType, ProtobufCMessage *);
static PurpleCmdRet handle_join_cmd(PurpleConversation *, gchar *, gchar **, gchar **, MumbleProtocolData *);
static PurpleCmdRet handle_channels_cmd(PurpleConversation *, gchar *, gchar **, gchar **, MumbleProtocolData *);
static void register_cmd(MumbleProtocolData *, gchar *, gchar *, gchar *, PurpleCmdFunc);
static MumbleChannel *get_mumble_channel_by_id_string(MumbleChannelTree *, gchar *);
static void join_channel(PurpleConnection *, MumbleChannel *);
static GList *append_chat_entry(GList *, gchar *, gchar *, gboolean);

static void mumble_protocol_init(PurpleProtocol *protocol) {
  protocol->id   = PROTOCOL_ID;
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
  iface->keepalive              = mumble_protocol_server_iface_keepalive;
  iface->get_keepalive_interval = mumble_protocol_server_iface_get_keepalive_interval;
}

static void mumble_protocol_chat_iface_init(PurpleProtocolChatIface *iface) {
  iface->info          = mumble_protocol_chat_iface_info;
  iface->info_defaults = mumble_protocol_chat_iface_info_defaults;
  iface->join          = mumble_protocol_chat_iface_join;
  iface->leave         = mumble_protocol_chat_iface_leave;
  iface->send          = mumble_protocol_chat_iface_send;
}

static void mumble_protocol_roomlist_iface_init(PurpleProtocolRoomlistIface *iface) {
  iface->get_list = mumble_protocol_roomlist_iface_get_list;
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

  for (GList *node = protocolData->registeredCmds; node; node = node->next) {
    purple_cmd_unregister(GPOINTER_TO_INT(node->data));
  }

  purple_account_set_status(purple_connection_get_account(connection), "offline", TRUE, NULL);

  g_cancellable_cancel(protocolData->cancellable);
  g_clear_object(&protocolData->cancellable);

  purple_gio_graceful_close(G_IO_STREAM(protocolData->connection), G_INPUT_STREAM(protocolData->inputStream), G_OUTPUT_STREAM(protocolData->outputStream));

  g_clear_object(&protocolData->inputStream);
  g_clear_object(&protocolData->outputStream);
  g_clear_object(&protocolData->connection);

  if (protocolData->tree) {
    mumble_channel_tree_free(protocolData->tree);
  }

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

static void mumble_protocol_server_iface_keepalive(PurpleConnection *connection) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  MumbleProto__Ping pingMessage = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocolData, MUMBLE_PING, (ProtobufCMessage *) &pingMessage);
}

static int mumble_protocol_server_iface_get_keepalive_interval() {
  return 10;
}

static GList *mumble_protocol_chat_iface_info(PurpleConnection *connection) {
  GList *entries = NULL;

  entries = append_chat_entry(entries, _("Channel:"), "channel", FALSE);
  entries = append_chat_entry(entries, _("ID"), "id", FALSE);

  return entries;
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

  gchar *channelName = g_hash_table_lookup(components, "channel");
  gchar *idString    = g_hash_table_lookup(components, "id");

  MumbleChannel *channel;
  if (idString && strlen(idString)) {
    channel = get_mumble_channel_by_id_string(protocolData->tree, idString);
  } else {
    channel = mumble_channel_tree_get_channel_by_name(protocolData->tree, channelName);
  }

  if (channel) {
    join_channel(connection, channel);
  } else {
    gchar *error = g_strdup_printf(_("%s is not a valid channel name"), channelName);
    purple_notify_error(connection, _("Invalid channel name"), _("Invalid channel name"), error, NULL);
    g_free(error);

    purple_serv_got_join_chat_failed(connection, components);
  }
}

static void mumble_protocol_chat_iface_leave(PurpleConnection *connection, int id) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  purple_serv_got_chat_left(connection, id);
  protocolData->activeChat = NULL;
}

static int mumble_protocol_chat_iface_send(PurpleConnection *connection, int id, PurpleMessage *message) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  MumbleProto__TextMessage textMessage = MUMBLE_PROTO__TEXT_MESSAGE__INIT;
  int ids[1] = { mumble_channel_tree_get_user_channel_id(protocolData->tree, protocolData->sessionId) };
  textMessage.has_actor = 0;
  textMessage.n_channel_id = 1;
  textMessage.channel_id = ids;
  textMessage.message = purple_message_get_contents(message);
  write_mumble_message(protocolData, MUMBLE_TEXT_MESSAGE, (ProtobufCMessage *) &textMessage);

  purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocolData->activeChat), protocolData->userName, purple_message_get_flags(message), purple_message_get_contents(message), time(NULL));

  return 0;
}

static PurpleRoomlist *mumble_protocol_roomlist_iface_get_list(PurpleConnection *connection) {
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  PurpleRoomlist *roomlist = purple_roomlist_new(purple_connection_get_account(connection));

  GList *fields = NULL;
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "channel", TRUE));
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, _("Description"), "description", FALSE));
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, _("ID"), "id", FALSE));

  purple_roomlist_set_fields(roomlist, fields);

  /*
   * Mumble has a hierarchy of channels, so link the rooms to a tree structure.
   */
  GHashTable *channelIdToRoom = g_hash_table_new(g_int_hash, g_int_equal);
  GList *channels = mumble_channel_tree_get_channels_in_topological_order(protocolData->tree);

  for (GList *node = channels; node; node = node->next) {
    MumbleChannel *channel = node->data;
    guint parentId = mumble_channel_tree_get_parent_id(protocolData->tree, channel->id);

    PurpleRoomlistRoom *parentRoom = g_hash_table_lookup(channelIdToRoom, &parentId);

    PurpleRoomlistRoomType type = PURPLE_ROOMLIST_ROOMTYPE_ROOM;
    if (mumble_channel_tree_has_children(protocolData->tree, channel->id)) {
      type |= PURPLE_ROOMLIST_ROOMTYPE_CATEGORY;
    }

    PurpleRoomlistRoom *room = purple_roomlist_room_new(type, channel->name, parentRoom);

    purple_roomlist_room_add_field(roomlist, room, channel->name);
    purple_roomlist_room_add_field(roomlist, room, channel->description);
    purple_roomlist_room_add_field(roomlist, room, GINT_TO_POINTER(channel->id));

    purple_roomlist_room_add(roomlist, room);

    g_hash_table_insert(channelIdToRoom, &channel->id, room);

    purple_debug_info("mumble", "Adding channel '%s' to roomlist", channel->name);
  }

  g_hash_table_destroy(channelIdToRoom);
  g_list_free(channels);

  return roomlist;
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

  register_cmd(protocolData, "join", "w", _("join &lt;channel name&gt;:  Join a channel"), handle_join_cmd);
  register_cmd(protocolData, "join-id", "w", _("join-id &lt;channel ID&gt;:  Join a channel"), handle_join_cmd);
  register_cmd(protocolData, "channels", "", _("channels:  List channels"), handle_channels_cmd);

  protocolData->tree = mumble_channel_tree_new();
  protocolData->sessionId = -1;

  protocolData->outputStream = mumble_output_stream_new(g_io_stream_get_output_stream(G_IO_STREAM(protocolData->connection)));
  protocolData->inputStream  = mumble_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(protocolData->connection)));

  protocolData->cancellable = g_cancellable_new();

  mumble_input_stream_read_message_async(protocolData->inputStream, protocolData->cancellable, on_read, purpleConnection);

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

  purple_connection_set_state(purpleConnection, PURPLE_CONNECTION_CONNECTED);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  GError *error = NULL;
  MumbleMessage *message = mumble_input_stream_read_message_finish(protocolData->inputStream, result, &error);
  if (error) {
    purple_connection_take_error(connection, error);
    return;
  }

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

      MumbleChannel *channel = mumble_channel_tree_get_channel(protocolData->tree, channelState->channel_id);
      if (channel) {
        if (channelState->name) {
          mumble_channel_set_name(channel, channelState->name);
          purple_debug_info("mumble", "Setting channel name to '%s'", channel->name);
        }
        for (int index = 0; index < channelState->n_links_remove; index++) {
          mumble_channel_tree_remove_subtree(protocolData->tree, channelState->links_remove[index]);
          purple_debug_info("mumble", "Removing subtree %d", channelState->links_remove[index]);
        }
      } else {
        channel = mumble_channel_new(channelState->channel_id, channelState->name, channelState->description);
        mumble_channel_tree_add_channel(protocolData->tree, channel, channelState->parent);
        purple_debug_info("mumble", "Adding channel %d->%d", channelState->parent, channelState->channel_id);
      }
      break;
    }
    case MUMBLE_USER_REMOVE: {
      MumbleProto__UserRemove *userRemove = (MumbleProto__UserRemove *) message->payload;

      purple_debug_info("mumble", "MUMBLE_USER_REMOVE: session=%d actor=%d reason=%s ban=%d", userRemove->session, userRemove->actor, userRemove->reason, userRemove->ban);

      MumbleUser *user = mumble_channel_tree_get_user(protocolData->tree, userRemove->session);
      if (user) {
        if (protocolData->activeChat) {
          if (user->channelId == mumble_channel_tree_get_user_channel_id(protocolData->tree, protocolData->sessionId)) {
            purple_chat_conversation_remove_user(protocolData->activeChat, user->name, NULL);
          }
        }
        mumble_channel_tree_remove_user(protocolData->tree, user->sessionId);
      }
      break;
    }
    case MUMBLE_USER_STATE: {
      MumbleProto__UserState *userState = (MumbleProto__UserState *) message->payload;

      purple_debug_info("mumble", "MUMBLE_USER_STATE: session=%d name=%s has_channel_id=%d channel_id=%d mute=%d deaf=%d suppress=%d self_mute=%d self_deaf=%d has_texture=%d comment=%s hash=%s has_comment_hash=%d has_texture_hash=%d priority_speaker=%d recording=%d",
        userState->session, userState->name, userState->has_channel_id, userState->channel_id, userState->mute, userState->deaf, userState->suppress, userState->self_mute, userState->self_deaf, userState->has_texture, userState->comment, userState->hash, userState->has_comment_hash,
        userState->has_texture_hash, userState->priority_speaker, userState->recording);

      MumbleUser *user = mumble_channel_tree_get_user(protocolData->tree, userState->session);
      if (user) {
        purple_debug_info("mumble", "Modifying user name='%s' sessionId=%d channelId=%d", user->name, user->sessionId, user->channelId);
        if (userState->has_channel_id && (userState->channel_id != user->channelId)) {
          if (userState->session == protocolData->sessionId) {
            join_channel(connection, mumble_channel_tree_get_channel(protocolData->tree, userState->channel_id));
          } else {
            if (protocolData->activeChat) {
              guint channelId = mumble_channel_tree_get_user_channel_id(protocolData->tree, protocolData->sessionId);
              if (user->channelId == channelId) {
                purple_chat_conversation_remove_user(protocolData->activeChat, user->name, NULL);
              } else if (userState->channel_id == channelId) {
                purple_chat_conversation_add_user(protocolData->activeChat, user->name, NULL, 0, FALSE);
              }
            }
          }
          
          user->channelId = userState->channel_id;
        }
        purple_debug_info("mumble", "Modified user name='%s' sessionId=%d channelId=%d", user->name, user->sessionId, user->channelId);
      } else {
        user = mumble_user_new(userState->session, userState->name, userState->channel_id);
        mumble_channel_tree_add_user(protocolData->tree, user);

        if (!g_strcmp0(protocolData->userName, user->name)) {
          protocolData->sessionId = user->sessionId;
        }

        if (protocolData->activeChat) {
          if (user->channelId == mumble_channel_tree_get_user_channel_id(protocolData->tree, protocolData->sessionId)) {
            purple_chat_conversation_add_user(protocolData->activeChat, user->name, NULL, 0, FALSE);
          }
        }

        purple_debug_info("mumble", "Adding user name='%s' sessionId=%d channelId=%d", user->name, user->sessionId, user->channelId);
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

      MumbleUser *actor = mumble_channel_tree_get_user(protocolData->tree, textMessage->actor);

      purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocolData->activeChat), actor->name, 0, textMessage->message, time(NULL));
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

  mumble_input_stream_read_message_async(protocolData->inputStream, protocolData->cancellable, on_read, connection);
}

static PurpleCmdRet handle_join_cmd(PurpleConversation *conversation, gchar *cmd, gchar **args, gchar **error, MumbleProtocolData *protocolData) {
  MumbleChannel *channel;
  if (!g_strcmp0(cmd, "join")) {
    channel = mumble_channel_tree_get_channel_by_name(protocolData->tree, args[0]);
  } else {
    channel = get_mumble_channel_by_id_string(protocolData->tree, args[0]);
  }

  if (!channel) {
    *error = g_strdup(_("No such channel"));
    return PURPLE_CMD_RET_FAILED;
  }

  join_channel(purple_conversation_get_connection(conversation), channel);

  return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet handle_channels_cmd(PurpleConversation *conversation, gchar *cmd, gchar **args, gchar **error, MumbleProtocolData *protocolData) {
  GList *channels = mumble_channel_tree_get_channels_in_topological_order(protocolData->tree);

  GString *message = g_string_new(NULL);
  for (GList *node = channels; node; node = node->next) {
    MumbleChannel *channel = node->data;
    int parentId = mumble_channel_tree_get_parent_id(protocolData->tree, channel->id);

    g_string_append_with_delimiter(message, g_strdup_printf(_("Name: %s"), channel->name), "<br><br>");
    g_string_append_with_delimiter(message, g_strdup_printf(_("Description: %s"), channel->description), "<br>");
    g_string_append_with_delimiter(message, g_strdup_printf(_("ID: %d"), channel->id), "<br>");
    if (parentId >= 0) {
      g_string_append_with_delimiter(message, g_strdup_printf(_("Parent: %d"), parentId), "<br>");
    }
  }

  purple_conversation_write_system_message(conversation, message->str, 0);

  g_list_free(channels);
  g_string_free(message, NULL);

  return PURPLE_CMD_RET_OK;
}

static void register_cmd(MumbleProtocolData *protocolData, gchar *name, gchar *args, gchar *help, PurpleCmdFunc func) {
  void *id = GINT_TO_POINTER(purple_cmd_register(name, args, PURPLE_CMD_P_PROTOCOL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PROTOCOL_ONLY, PROTOCOL_ID, func, help, protocolData));
  protocolData->registeredCmds = g_list_append(protocolData->registeredCmds, id);
}

static MumbleChannel *get_mumble_channel_by_id_string(MumbleChannelTree *tree, gchar *idString) {
  return mumble_channel_tree_get_channel(tree, g_ascii_strtoull(idString, NULL, 10));
}

/*
 * The user is always on a single channel, so there is a maximum of 1 active conversations at
 * a time. When the user joins a new channel, the conversation that they're leaving becomes
 * inactive.
 */
static void join_channel(PurpleConnection *connection, MumbleChannel *channel) {
  static int chatIdCounter = 0;

  MumbleProtocolData *protocolData = purple_connection_get_protocol_data(connection);

  gboolean alreadyJoined = channel->id == mumble_channel_tree_get_user_channel_id(protocolData->tree, protocolData->sessionId);
  gboolean hasActiveChat = protocolData->activeChat;

  if (!(alreadyJoined && hasActiveChat)) {
    if (!alreadyJoined) {
      mumble_channel_tree_set_user_channel_id(protocolData->tree, protocolData->sessionId, channel->id);

      MumbleProto__UserState userState = MUMBLE_PROTO__USER_STATE__INIT;
      userState.has_session = TRUE;
      userState.session = protocolData->sessionId;
      userState.has_actor = FALSE;
      userState.has_channel_id = TRUE;
      userState.channel_id = channel->id;
      write_mumble_message(protocolData, MUMBLE_USER_STATE, (ProtobufCMessage *) &userState);
    }

    if (hasActiveChat) {
      purple_chat_conversation_remove_user(protocolData->activeChat, protocolData->userName, NULL);
      purple_serv_got_chat_left(connection, purple_chat_conversation_get_id(protocolData->activeChat));
    }

    protocolData->activeChat = purple_serv_got_joined_chat(connection, chatIdCounter++, channel->name);

    GList *names = mumble_channel_tree_get_channel_user_names(protocolData->tree, channel->id);
    GList *flags = g_list_append_times(NULL, GINT_TO_POINTER(PURPLE_CHAT_USER_NONE), g_list_length(names));
    purple_chat_conversation_add_users(protocolData->activeChat, names, NULL, flags, FALSE);
    g_list_free(names);
  }
}

static GList *append_chat_entry(GList *entries, gchar *label, gchar *identifier, gboolean required) {
  PurpleProtocolChatEntry *entry;

  entry = g_new0(PurpleProtocolChatEntry, 1);
  entry->label      = label;
  entry->identifier = identifier;
  entry->required   = required;

  return g_list_append(entries, entry);
}

static void write_mumble_message(MumbleProtocolData *protocolData, MumbleMessageType type, ProtobufCMessage *payload) {
  MumbleMessage *message = mumble_message_new(type, payload);
  mumble_output_stream_write_message_async(protocolData->outputStream, message, protocolData->cancellable, NULL, NULL);
}
