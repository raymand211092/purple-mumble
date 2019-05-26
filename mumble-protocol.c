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
  MumbleInputStream *input_stream;
  MumbleOutputStream *output_stream;
  GCancellable *cancellable;
  gchar *user_name;
  gchar *server;
  GList *registered_cmds;
  PurpleChatConversation *active_chat;
  MumbleChannelTree *tree;
  guint session_id;
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

static void mumble_protocol_class_init(PurpleProtocolClass *protocol_class) {
  protocol_class->login        = mumble_protocol_login;
  protocol_class->close        = mumble_protocol_close;
  protocol_class->status_types = mumble_protocol_status_types;
  protocol_class->list_icon    = mumble_protocol_list_icon;
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
  
  MumbleProtocolData *protocol_data = g_new0(MumbleProtocolData, 1);
  purple_connection_set_protocol_data(connection, protocol_data);
  
  gchar **parts = g_strsplit(purple_account_get_username(account), "@", 2);
  protocol_data->user_name = g_strdup(parts[0]);
  protocol_data->server = g_strdup(parts[1]);
  g_strfreev(parts);
  
  GError *error;
  GSocketClient *client = purple_gio_socket_client_new(account, &error);
  if (!client) {
    purple_connection_take_error(connection, error);
    return;
  }
  g_socket_client_set_tls(client, TRUE);
  g_socket_client_set_tls_validation_flags(client, 0);
  
  g_socket_client_connect_to_host_async(client, protocol_data->server, purple_account_get_int(account, "port", 64738), NULL, on_connected, connection);
  
  g_object_unref(client);
  
  purple_connection_set_state(connection, PURPLE_CONNECTION_CONNECTING);
}

static void mumble_protocol_close(PurpleConnection *connection) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  for (GList *node = protocol_data->registered_cmds; node; node = node->next) {
    purple_cmd_unregister(GPOINTER_TO_INT(node->data));
  }

  purple_account_set_status(purple_connection_get_account(connection), "offline", TRUE, NULL);

  g_cancellable_cancel(protocol_data->cancellable);
  g_clear_object(&protocol_data->cancellable);

  purple_gio_graceful_close(G_IO_STREAM(protocol_data->connection), G_INPUT_STREAM(protocol_data->input_stream), G_OUTPUT_STREAM(protocol_data->output_stream));

  g_clear_object(&protocol_data->input_stream);
  g_clear_object(&protocol_data->output_stream);
  g_clear_object(&protocol_data->connection);

  if (protocol_data->tree) {
    mumble_channel_tree_free(protocol_data->tree);
  }

  g_free(protocol_data->user_name);
  g_free(protocol_data->server);
  g_free(protocol_data);
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
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  MumbleProto__Ping ping_message = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocol_data, MUMBLE_PING, (ProtobufCMessage *) &ping_message);
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

static GHashTable *mumble_protocol_chat_iface_info_defaults(PurpleConnection *connection, const char *chat_name) {
  GHashTable *defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  
  if (chat_name) {
    g_hash_table_insert(defaults, "channel", g_strdup(chat_name));
  }
  
  return defaults;
}

static void mumble_protocol_chat_iface_join(PurpleConnection *connection, GHashTable *components) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  gchar *channel_name = g_hash_table_lookup(components, "channel");
  gchar *id_string    = g_hash_table_lookup(components, "id");

  MumbleChannel *channel;
  if (id_string && strlen(id_string)) {
    channel = get_mumble_channel_by_id_string(protocol_data->tree, id_string);
  } else {
    channel = mumble_channel_tree_get_channel_by_name(protocol_data->tree, channel_name);
  }

  if (channel) {
    join_channel(connection, channel);
  } else {
    gchar *error = g_strdup_printf(_("%s is not a valid channel name"), channel_name);
    purple_notify_error(connection, _("Invalid channel name"), _("Invalid channel name"), error, NULL);
    g_free(error);

    purple_serv_got_join_chat_failed(connection, components);
  }
}

static void mumble_protocol_chat_iface_leave(PurpleConnection *connection, int id) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  purple_serv_got_chat_left(connection, id);
  protocol_data->active_chat = NULL;
}

static int mumble_protocol_chat_iface_send(PurpleConnection *connection, int id, PurpleMessage *message) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  MumbleProto__TextMessage text_message = MUMBLE_PROTO__TEXT_MESSAGE__INIT;
  int ids[1] = { mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id) };
  text_message.has_actor = 0;
  text_message.n_channel_id = 1;
  text_message.channel_id = ids;
  text_message.message = purple_message_get_contents(message);
  write_mumble_message(protocol_data, MUMBLE_TEXT_MESSAGE, (ProtobufCMessage *) &text_message);

  purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocol_data->active_chat), protocol_data->user_name, purple_message_get_flags(message), purple_message_get_contents(message), time(NULL));

  return 0;
}

static PurpleRoomlist *mumble_protocol_roomlist_iface_get_list(PurpleConnection *connection) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  PurpleRoomlist *roomlist = purple_roomlist_new(purple_connection_get_account(connection));

  GList *fields = NULL;
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "channel", TRUE));
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, _("Description"), "description", FALSE));
  fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, _("ID"), "id", FALSE));

  purple_roomlist_set_fields(roomlist, fields);

  /*
   * Mumble has a hierarchy of channels, so link the rooms to a tree structure.
   */
  GHashTable *channel_id_to_room = g_hash_table_new(g_int_hash, g_int_equal);
  GList *channels = mumble_channel_tree_get_channels_in_topological_order(protocol_data->tree);

  for (GList *node = channels; node; node = node->next) {
    MumbleChannel *channel = node->data;
    guint parent_id = mumble_channel_tree_get_parent_id(protocol_data->tree, channel->id);

    PurpleRoomlistRoom *parent_room = g_hash_table_lookup(channel_id_to_room, &parent_id);

    PurpleRoomlistRoomType type = PURPLE_ROOMLIST_ROOMTYPE_ROOM;
    if (mumble_channel_tree_has_children(protocol_data->tree, channel->id)) {
      type |= PURPLE_ROOMLIST_ROOMTYPE_CATEGORY;
    }

    PurpleRoomlistRoom *room = purple_roomlist_room_new(type, channel->name, parent_room);

    purple_roomlist_room_add_field(roomlist, room, channel->name);
    purple_roomlist_room_add_field(roomlist, room, channel->description);
    purple_roomlist_room_add_field(roomlist, room, GINT_TO_POINTER(channel->id));

    purple_roomlist_room_add(roomlist, room);

    g_hash_table_insert(channel_id_to_room, &channel->id, room);

    purple_debug_info("mumble", "Adding channel '%s' to roomlist", channel->name);
  }

  g_hash_table_destroy(channel_id_to_room);
  g_list_free(channels);

  return roomlist;
}

static void on_connected(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *purple_connection = data;
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(purple_connection);

  GError *error = NULL;
  protocol_data->connection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, &error);
  if (error) {
    purple_connection_take_error(purple_connection, error);
    return;
  }

  register_cmd(protocol_data, "join", "w", _("join &lt;channel name&gt;:  Join a channel"), handle_join_cmd);
  register_cmd(protocol_data, "join-id", "w", _("join-id &lt;channel ID&gt;:  Join a channel"), handle_join_cmd);
  register_cmd(protocol_data, "channels", "", _("channels:  List channels"), handle_channels_cmd);

  protocol_data->tree = mumble_channel_tree_new();
  protocol_data->session_id = -1;

  protocol_data->output_stream = mumble_output_stream_new(g_io_stream_get_output_stream(G_IO_STREAM(protocol_data->connection)));
  protocol_data->input_stream  = mumble_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(protocol_data->connection)));

  protocol_data->cancellable = g_cancellable_new();

  mumble_input_stream_read_message_async(protocol_data->input_stream, protocol_data->cancellable, on_read, purple_connection);

  MumbleProto__Version version_message = MUMBLE_PROTO__VERSION__INIT;
  version_message.has_version = 1;
  version_message.version = 0x010213;
  version_message.release = "foo";
  version_message.os = "bar";
  version_message.os_version = "baz";
  write_mumble_message(protocol_data, MUMBLE_VERSION, (ProtobufCMessage *) &version_message);

  MumbleProto__Authenticate authenticate_message = MUMBLE_PROTO__AUTHENTICATE__INIT;
  authenticate_message.username = protocol_data->user_name;
  write_mumble_message(protocol_data, MUMBLE_AUTHENTICATE, (ProtobufCMessage *) &authenticate_message);

  MumbleProto__Ping ping_message = MUMBLE_PROTO__PING__INIT;
  write_mumble_message(protocol_data, MUMBLE_PING, (ProtobufCMessage *) &ping_message);

  purple_connection_set_state(purple_connection, PURPLE_CONNECTION_CONNECTED);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  GError *error = NULL;
  MumbleMessage *message = mumble_input_stream_read_message_finish(protocol_data->input_stream, result, &error);
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
      MumbleProto__ServerSync *server_sync = (MumbleProto__ServerSync *) message->payload;
      purple_debug_info("mumble", "MUMBLE_SERVER_SYNC: session=%d max_badwidth=%d welcome_text=%s permissions=%d", server_sync->session, server_sync->max_bandwidth, server_sync->welcome_text, server_sync->permissions);
      break;
    }
    case MUMBLE_CHANNEL_REMOVE: {
      MumbleProto__ChannelRemove *channel_remove = (MumbleProto__ChannelRemove *) message->payload;
      purple_debug_info("mumble", "MUMBLE_CHANNEL_REMOVE: channel_id=%d", channel_remove->channel_id);
      break;
    }
    case MUMBLE_CHANNEL_STATE: {
      MumbleProto__ChannelState *channel_state = (MumbleProto__ChannelState *) message->payload;

      purple_debug_info("mumble", "MUMBLE_CHANNEL_STATE: channel_id=%d parent=%d name=%s description=%s temporary=%d position=%d max_users=%d", channel_state->channel_id, channel_state->parent, channel_state->name, channel_state->description,
        channel_state->has_temporary ? channel_state->temporary : -1, channel_state->has_position ? channel_state->position : -1, channel_state->has_max_users ? channel_state->max_users : -1);
      purple_debug_info("mumble", "Links:");
      for (int index = 0; index < channel_state->n_links; index++) {
        purple_debug_info("mumble", "%d", channel_state->links[index]);
      }
      purple_debug_info("mumble", "Links to add:");
      for (int index = 0; index < channel_state->n_links_add; index++) {
        purple_debug_info("mumble", "%d", channel_state->links_add[index]);
      }
      purple_debug_info("mumble", "Links to remove:");
      for (int index = 0; index < channel_state->n_links_remove; index++) {
        purple_debug_info("mumble", "%d", channel_state->links_remove[index]);
      }

      MumbleChannel *channel = mumble_channel_tree_get_channel(protocol_data->tree, channel_state->channel_id);
      if (channel) {
        if (channel_state->name) {
          mumble_channel_set_name(channel, channel_state->name);
          purple_debug_info("mumble", "Setting channel name to '%s'", channel->name);
        }
        for (int index = 0; index < channel_state->n_links_remove; index++) {
          mumble_channel_tree_remove_subtree(protocol_data->tree, channel_state->links_remove[index]);
          purple_debug_info("mumble", "Removing subtree %d", channel_state->links_remove[index]);
        }
      } else {
        channel = mumble_channel_new(channel_state->channel_id, channel_state->name, channel_state->description);
        mumble_channel_tree_add_channel(protocol_data->tree, channel, channel_state->parent);
        purple_debug_info("mumble", "Adding channel %d->%d", channel_state->parent, channel_state->channel_id);
      }
      break;
    }
    case MUMBLE_USER_REMOVE: {
      MumbleProto__UserRemove *user_remove = (MumbleProto__UserRemove *) message->payload;

      purple_debug_info("mumble", "MUMBLE_USER_REMOVE: session=%d actor=%d reason=%s ban=%d", user_remove->session, user_remove->actor, user_remove->reason, user_remove->ban);

      MumbleUser *user = mumble_channel_tree_get_user(protocol_data->tree, user_remove->session);
      if (user) {
        if (protocol_data->active_chat) {
          if (user->channel_id == mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id)) {
            purple_chat_conversation_remove_user(protocol_data->active_chat, user->name, NULL);
          }
        }
        mumble_channel_tree_remove_user(protocol_data->tree, user->session_id);
      }
      break;
    }
    case MUMBLE_USER_STATE: {
      MumbleProto__UserState *user_state = (MumbleProto__UserState *) message->payload;

      purple_debug_info("mumble", "MUMBLE_USER_STATE: session=%d name=%s has_channel_id=%d channel_id=%d mute=%d deaf=%d suppress=%d self_mute=%d self_deaf=%d has_texture=%d comment=%s hash=%s has_comment_hash=%d has_texture_hash=%d priority_speaker=%d recording=%d",
        user_state->session, user_state->name, user_state->has_channel_id, user_state->channel_id, user_state->mute, user_state->deaf, user_state->suppress, user_state->self_mute, user_state->self_deaf, user_state->has_texture, user_state->comment, user_state->hash, user_state->has_comment_hash,
        user_state->has_texture_hash, user_state->priority_speaker, user_state->recording);

      MumbleUser *user = mumble_channel_tree_get_user(protocol_data->tree, user_state->session);
      if (user) {
        purple_debug_info("mumble", "Modifying user name='%s' session_id=%d channel_id=%d", user->name, user->session_id, user->channel_id);
        if (user_state->has_channel_id && (user_state->channel_id != user->channel_id)) {
          if (user_state->session == protocol_data->session_id) {
            join_channel(connection, mumble_channel_tree_get_channel(protocol_data->tree, user_state->channel_id));
          } else {
            if (protocol_data->active_chat) {
              guint channel_id = mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id);
              if (user->channel_id == channel_id) {
                purple_chat_conversation_remove_user(protocol_data->active_chat, user->name, NULL);
              } else if (user_state->channel_id == channel_id) {
                purple_chat_conversation_add_user(protocol_data->active_chat, user->name, NULL, 0, FALSE);
              }
            }
          }
          
          user->channel_id = user_state->channel_id;
        }
        purple_debug_info("mumble", "Modified user name='%s' session_id=%d channel_id=%d", user->name, user->session_id, user->channel_id);
      } else {
        user = mumble_user_new(user_state->session, user_state->name, user_state->channel_id);
        mumble_channel_tree_add_user(protocol_data->tree, user);

        if (!g_strcmp0(protocol_data->user_name, user->name)) {
          protocol_data->session_id = user->session_id;
        }

        if (protocol_data->active_chat) {
          if (user->channel_id == mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id)) {
            purple_chat_conversation_add_user(protocol_data->active_chat, user->name, NULL, 0, FALSE);
          }
        }

        purple_debug_info("mumble", "Adding user name='%s' session_id=%d channel_id=%d", user->name, user->session_id, user->channel_id);
      }
      break;
    }
    case MUMBLE_BAN_LIST: {
      MumbleProto__BanList *ban_list = (MumbleProto__BanList *) message->payload;

      purple_debug_info("mumble", "MUMBLE_BAN_LIST: query=%d", ban_list->query);
      for (int index = 0; index < ban_list->n_bans; index++) {
        MumbleProto__BanList__BanEntry *entry = ban_list->bans[index];
        purple_debug_info("mumble", "address= mask=%d name=%s hash=%s reason=%s start=%s duration=%d", entry->mask, entry->mask, entry->hash, entry->reason, entry->start, entry->duration);
      }
      break;
    }
    case MUMBLE_TEXT_MESSAGE: {
      MumbleProto__TextMessage *text_message = (MumbleProto__TextMessage *) message->payload;

      purple_debug_info("mumble", "MUMBLE_TEXT_MESSAGE: actor=%d message=%s", text_message->has_actor ? text_message->actor : -1, text_message->message);

      MumbleUser *actor = mumble_channel_tree_get_user(protocol_data->tree, text_message->actor);

      purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocol_data->active_chat), actor->name, 0, text_message->message, time(NULL));
      break;
    }
    case MUMBLE_PERMISSION_DENIED: {
      MumbleProto__PermissionDenied *permission_denied = (MumbleProto__PermissionDenied *) message->payload;
      purple_debug_info("mumble", "MUMBLE_PERMISSION_DENIED: permission=%d channel_id=%d session=%d reason=%s type=%d name=%s", permission_denied->permission, permission_denied->channel_id, permission_denied->session, permission_denied->reason,
        permission_denied->type, permission_denied->name);
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
        for (int user_index = 0; user_index < group->n_add; user_index++) {
          purple_debug_info("mumble", "%d", group->add[user_index]);
        }
        purple_debug_info("mumble", "Users explicitly removed from this group:");
        for (int user_index = 0; user_index < group->n_remove; user_index++) {
          purple_debug_info("mumble", "%d", group->remove[user_index]);
        }
        purple_debug_info("mumble", "Users inherited:");
        for (int user_index = 0; user_index < group->n_inherited_members; user_index++) {
          purple_debug_info("mumble", "%d", group->inherited_members[user_index]);
        }
      }
      purple_debug_info("mumble", "ACL specifications");
      for (int index = 0; index < acl->n_acls; index++) {
        MumbleProto__ACL__ChanACL *channel_acl = acl->acls[index];
        purple_debug_info("mumble", "apply_here=%d apply_subs=%d inherited=%d user_id=%d group=%s grant=%d deny=%d", channel_acl->apply_here, channel_acl->apply_subs, channel_acl->inherited, channel_acl->user_id, channel_acl->group, channel_acl->grant, channel_acl->deny);
      }
      break;
    }
    case MUMBLE_PERMISSION_QUERY: {
      MumbleProto__PermissionQuery *permission_query = (MumbleProto__PermissionQuery *) message->payload;
      purple_debug_info("mumble", "MUMBLE_PERMISSION_QUERY: channel_id=%d permissions=%d flush=%d", permission_query->channel_id, permission_query->permissions, permission_query->flush);
      break;
    }
    case MUMBLE_CODEC_VERSION: {
      MumbleProto__CodecVersion *codec_version = (MumbleProto__CodecVersion *) message->payload;
      purple_debug_info("mumble", "MUMBLE_CODEC_VERSION: alpha=%d beta=%d prefer_alpha=%d opus=%d", codec_version->alpha, codec_version->beta, codec_version->prefer_alpha, codec_version->opus);
      break;
    }
    case MUMBLE_USER_STATS: {
      MumbleProto__UserStats *user_stats = (MumbleProto__UserStats *) message->payload;
      purple_debug_info("mumble", "MUMBLE_USER_STATS: session=%d stats_only=%d n_certificates=%d udp_packets=%d tcp_packets=%d udp_ping_avg=%f udp_ping_var=%f tcp_ping_avg=%f tcp_ping_var=%f n_celt_versions=%d address= bandwidth=%d onlinesecs=%d idlesecs=%d strong_certificate=%d opus=%d",
        user_stats->session, user_stats->stats_only, user_stats->udp_packets, user_stats->tcp_packets, user_stats->udp_ping_avg, user_stats->udp_ping_var, user_stats->tcp_ping_avg, user_stats->tcp_ping_var, user_stats->n_celt_versions, user_stats->bandwidth,
        user_stats->onlinesecs, user_stats->idlesecs, user_stats->strong_certificate, user_stats->opus);
      purple_debug_info("mumble", "Statistics for packets received from client: good=%d late=%d lost=%d resync=%d", user_stats->from_client->good, user_stats->from_client->late, user_stats->from_client->lost, user_stats->from_client->resync);
      purple_debug_info("mumble", "Statistics for packets sent by server: good=%d late=%d lost=%d resync=%d", user_stats->from_server->good, user_stats->from_server->late, user_stats->from_server->lost, user_stats->from_server->resync);
      break;
    }
    case MUMBLE_SERVER_CONFIG: {
      MumbleProto__ServerConfig *server_config = (MumbleProto__ServerConfig *) message->payload;
      purple_debug_info("mumble", "MUMBLE_SERVER_CONFIG: max_bandwidth=%d welcome_text=%s allow_html=%d message_length=%d image_message_length=%d max_users=%d", server_config->max_bandwidth, server_config->welcome_text, server_config->allow_html,
        server_config->message_length, server_config->image_message_length, server_config->max_users);
      break;
    }
    case MUMBLE_SUGGEST_CONFIG: {
      MumbleProto__SuggestConfig *suggest_config = (MumbleProto__SuggestConfig *) message->payload;
      purple_debug_info("mumble", "MUMBLE_SUGGEST_CONFIG: version=%d positional=%d push_to_talk=%d", suggest_config->version, suggest_config->positional, suggest_config->push_to_talk);
      break;
    }
    default: {
      purple_debug_info("mumble", "Read message of type %d", message->type);
      break;
    }
  }

  mumble_message_free(message);

  mumble_input_stream_read_message_async(protocol_data->input_stream, protocol_data->cancellable, on_read, connection);
}

static PurpleCmdRet handle_join_cmd(PurpleConversation *conversation, gchar *cmd, gchar **args, gchar **error, MumbleProtocolData *protocol_data) {
  MumbleChannel *channel;
  if (!g_strcmp0(cmd, "join")) {
    channel = mumble_channel_tree_get_channel_by_name(protocol_data->tree, args[0]);
  } else {
    channel = get_mumble_channel_by_id_string(protocol_data->tree, args[0]);
  }

  if (!channel) {
    *error = g_strdup(_("No such channel"));
    return PURPLE_CMD_RET_FAILED;
  }

  join_channel(purple_conversation_get_connection(conversation), channel);

  return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet handle_channels_cmd(PurpleConversation *conversation, gchar *cmd, gchar **args, gchar **error, MumbleProtocolData *protocol_data) {
  GList *channels = mumble_channel_tree_get_channels_in_topological_order(protocol_data->tree);

  GString *message = g_string_new(NULL);
  for (GList *node = channels; node; node = node->next) {
    MumbleChannel *channel = node->data;
    int parent_id = mumble_channel_tree_get_parent_id(protocol_data->tree, channel->id);

    g_string_append_with_delimiter(message, g_strdup_printf(_("Name: %s"), channel->name), "<br><br>");
    g_string_append_with_delimiter(message, g_strdup_printf(_("Description: %s"), channel->description), "<br>");
    g_string_append_with_delimiter(message, g_strdup_printf(_("ID: %d"), channel->id), "<br>");
    if (parent_id >= 0) {
      g_string_append_with_delimiter(message, g_strdup_printf(_("Parent: %d"), parent_id), "<br>");
    }
  }

  purple_conversation_write_system_message(conversation, message->str, 0);

  g_list_free(channels);
  g_string_free(message, NULL);

  return PURPLE_CMD_RET_OK;
}

static void register_cmd(MumbleProtocolData *protocol_data, gchar *name, gchar *args, gchar *help, PurpleCmdFunc func) {
  void *id = GINT_TO_POINTER(purple_cmd_register(name, args, PURPLE_CMD_P_PROTOCOL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PROTOCOL_ONLY, PROTOCOL_ID, func, help, protocol_data));
  protocol_data->registered_cmds = g_list_append(protocol_data->registered_cmds, id);
}

static MumbleChannel *get_mumble_channel_by_id_string(MumbleChannelTree *tree, gchar *id_string) {
  return mumble_channel_tree_get_channel(tree, g_ascii_strtoull(id_string, NULL, 10));
}

/*
 * The user is always on a single channel, so there is a maximum of 1 active conversations at
 * a time. When the user joins a new channel, the conversation that they're leaving becomes
 * inactive.
 */
static void join_channel(PurpleConnection *connection, MumbleChannel *channel) {
  static int chat_id_counter = 0;

  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  gboolean already_joined = channel->id == mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id);
  gboolean has_active_chat = protocol_data->active_chat;

  if (!(already_joined && has_active_chat)) {
    if (!already_joined) {
      mumble_channel_tree_set_user_channel_id(protocol_data->tree, protocol_data->session_id, channel->id);

      MumbleProto__UserState user_state = MUMBLE_PROTO__USER_STATE__INIT;
      user_state.has_session = TRUE;
      user_state.session = protocol_data->session_id;
      user_state.has_actor = FALSE;
      user_state.has_channel_id = TRUE;
      user_state.channel_id = channel->id;
      write_mumble_message(protocol_data, MUMBLE_USER_STATE, (ProtobufCMessage *) &user_state);
    }

    if (has_active_chat) {
      purple_chat_conversation_remove_user(protocol_data->active_chat, protocol_data->user_name, NULL);
      purple_serv_got_chat_left(connection, purple_chat_conversation_get_id(protocol_data->active_chat));
    }

    protocol_data->active_chat = purple_serv_got_joined_chat(connection, chat_id_counter++, channel->name);

    GList *names = mumble_channel_tree_get_channel_user_names(protocol_data->tree, channel->id);
    GList *flags = g_list_append_times(NULL, GINT_TO_POINTER(PURPLE_CHAT_USER_NONE), g_list_length(names));
    purple_chat_conversation_add_users(protocol_data->active_chat, names, NULL, flags, FALSE);
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

static void write_mumble_message(MumbleProtocolData *protocol_data, MumbleMessageType type, ProtobufCMessage *payload) {
  MumbleMessage *message = mumble_message_new(type, payload);
  mumble_output_stream_write_message_async(protocol_data->output_stream, message, protocol_data->cancellable, NULL, NULL);
}
