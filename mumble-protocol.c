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

#include <glib/gi18n.h>
#include <purple.h>
#include "mumble-input-stream.h"
#include "mumble-output-stream.h"
#include "mumble-protocol.h"
#include "mumble-message.h"
#include "mumble-channel-tree.h"
#include "utils.h"
#include "protobuf-utils.h"
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

void mumble_protocol_register(PurplePlugin *);

static void mumble_protocol_init(MumbleProtocol *);
static void mumble_protocol_class_init(MumbleProtocolClass *);
static void mumble_protocol_class_finalize(MumbleProtocolClass *);

static void mumble_protocol_client_interface_init(PurpleProtocolClientInterface *);
static void mumble_protocol_server_interface_init(PurpleProtocolServerInterface *);
static void mumble_protocol_chat_interface_init(PurpleProtocolChatInterface *);
static void mumble_protocol_roomlist_interface_init(PurpleProtocolRoomlistInterface *);

static void mumble_protocol_login(PurpleAccount *);
static void mumble_protocol_close(PurpleConnection *);
static GList *mumble_protocol_status_types(PurpleAccount *);
static const char *mumble_protocol_list_icon(PurpleAccount *, PurpleBuddy *);

static gssize mumble_protocol_client_interface_get_max_message_size(PurpleConversation *);

static void mumble_protocol_server_interface_keepalive(PurpleConnection *connection);
static int mumble_protocol_server_interface_get_keepalive_interval();

static GList *mumble_protocol_chat_interface_info(PurpleConnection *);
static GHashTable *mumble_protocol_chat_interface_info_defaults(PurpleConnection *, const char *);
static void mumble_protocol_chat_interface_join(PurpleConnection *, GHashTable *);
static void mumble_protocol_chat_interface_leave(PurpleConnection *, int);
static int mumble_protocol_chat_interface_send(PurpleConnection *, int, PurpleMessage *);

static PurpleRoomlist *mumble_protocol_roomlist_interface_get_list(PurpleConnection *);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(MumbleProtocol, mumble_protocol, PURPLE_TYPE_PROTOCOL, 0,
  G_IMPLEMENT_INTERFACE_DYNAMIC(PURPLE_TYPE_PROTOCOL_CLIENT, mumble_protocol_client_interface_init)
  G_IMPLEMENT_INTERFACE_DYNAMIC(PURPLE_TYPE_PROTOCOL_SERVER, mumble_protocol_server_interface_init)
  G_IMPLEMENT_INTERFACE_DYNAMIC(PURPLE_TYPE_PROTOCOL_CHAT, mumble_protocol_chat_interface_init)
  G_IMPLEMENT_INTERFACE_DYNAMIC(PURPLE_TYPE_PROTOCOL_ROOMLIST, mumble_protocol_roomlist_interface_init));

G_MODULE_EXPORT GType mumble_protocol_get_type(void);

static void on_connected(GObject *, GAsyncResult *, gpointer);
static void on_read(GObject *, GAsyncResult *, gpointer);
static void write_mumble_message(MumbleProtocolData *, MumbleMessageType, GByteArray *);
static PurpleCmdRet handle_join_cmd(PurpleConversation *, gchar *, gchar **, gchar **, MumbleProtocolData *);
static PurpleCmdRet handle_channels_cmd(PurpleConversation *, gchar *, gchar **, gchar **, MumbleProtocolData *);
static void register_cmd(MumbleProtocolData *, gchar *, gchar *, gchar *, PurpleCmdFunc);
static MumbleChannel *get_mumble_channel_by_id_string(MumbleChannelTree *, gchar *);
static void join_channel(PurpleConnection *, MumbleChannel *);
static GList *append_chat_entry(GList *, gchar *, gchar *, gboolean);

void mumble_protocol_register(PurplePlugin *plugin) {
  mumble_protocol_register_type(plugin);
}

static void mumble_protocol_init(MumbleProtocol *mumble_protocol) {
  PurpleProtocol *protocol = PURPLE_PROTOCOL(mumble_protocol);

  protocol->id   = PROTOCOL_ID;
  protocol->name = "Mumble";

  protocol->options = OPT_PROTO_PASSWORD_OPTIONAL;

  protocol->user_splits = g_list_append(protocol->user_splits, purple_account_user_split_new(_("Server"), "localhost", '@'));

  protocol->account_options = g_list_append(protocol->account_options, purple_account_option_int_new(_("Port"), "port", 64738));
}

static void mumble_protocol_class_init(MumbleProtocolClass *mumble_protocol_class) {
  PurpleProtocolClass *protocol_class = PURPLE_PROTOCOL_CLASS(mumble_protocol_class);
  protocol_class->login        = mumble_protocol_login;
  protocol_class->close        = mumble_protocol_close;
  protocol_class->status_types = mumble_protocol_status_types;
  protocol_class->list_icon    = mumble_protocol_list_icon;
}

static void mumble_protocol_class_finalize(MumbleProtocolClass *mumble_protocol_class) {
  /* Empty. */
}

static void mumble_protocol_client_interface_init(PurpleProtocolClientInterface *interface) {
  interface->get_max_message_size = mumble_protocol_client_interface_get_max_message_size;
}

static void mumble_protocol_server_interface_init(PurpleProtocolServerInterface *interface) {
  interface->keepalive              = mumble_protocol_server_interface_keepalive;
  interface->get_keepalive_interval = mumble_protocol_server_interface_get_keepalive_interval;
}

static void mumble_protocol_chat_interface_init(PurpleProtocolChatInterface *interface) {
  interface->info          = mumble_protocol_chat_interface_info;
  interface->info_defaults = mumble_protocol_chat_interface_info_defaults;
  interface->join          = mumble_protocol_chat_interface_join;
  interface->leave         = mumble_protocol_chat_interface_leave;
  interface->send          = mumble_protocol_chat_interface_send;
}

static void mumble_protocol_roomlist_interface_init(PurpleProtocolRoomlistInterface *interface) {
  interface->get_list = mumble_protocol_roomlist_interface_get_list;
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

  purple_connection_set_protocol_data(connection, 0);
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

static gssize mumble_protocol_client_interface_get_max_message_size(PurpleConversation *conversation) {
  return 256;
}

static void mumble_protocol_server_interface_keepalive(PurpleConnection *connection) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);
  write_mumble_message(protocol_data, MUMBLE_PING, g_byte_array_new());
}

static int mumble_protocol_server_interface_get_keepalive_interval() {
  return 10;
}

static GList *mumble_protocol_chat_interface_info(PurpleConnection *connection) {
  GList *entries = NULL;

  entries = append_chat_entry(entries, _("Channel:"), "channel", FALSE);
  entries = append_chat_entry(entries, _("ID"), "id", FALSE);

  return entries;
}

static GHashTable *mumble_protocol_chat_interface_info_defaults(PurpleConnection *connection, const char *chat_name) {
  GHashTable *defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  
  if (chat_name) {
    g_hash_table_insert(defaults, "channel", g_strdup(chat_name));
  }
  
  return defaults;
}

static void mumble_protocol_chat_interface_join(PurpleConnection *connection, GHashTable *components) {
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

static void mumble_protocol_chat_interface_leave(PurpleConnection *connection, int id) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  purple_serv_got_chat_left(connection, id);
  protocol_data->active_chat = NULL;
}

static int mumble_protocol_chat_interface_send(PurpleConnection *connection, int id, PurpleMessage *message) {
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  GByteArray *text_message_message = g_byte_array_new();
  encode_protobuf_unsigned_varint(text_message_message, 3, mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id));
  encode_protobuf_string(text_message_message, 5, purple_message_get_contents(message));
  write_mumble_message(protocol_data, MUMBLE_TEXT_MESSAGE, text_message_message);

  purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocol_data->active_chat), protocol_data->user_name, purple_message_get_flags(message), purple_message_get_contents(message), time(NULL));

  return 0;
}

static PurpleRoomlist *mumble_protocol_roomlist_interface_get_list(PurpleConnection *connection) {
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

  GByteArray *version_message = g_byte_array_new();
  encode_protobuf_unsigned_varint(version_message, 1, 0x010213);
  encode_protobuf_string(version_message, 2, "purple-mumble");
  encode_protobuf_string(version_message, 3, "dummy");
  encode_protobuf_string(version_message, 4, "dummy");
  write_mumble_message(protocol_data, MUMBLE_VERSION, version_message);

  GByteArray *authenticate_message = g_byte_array_new();
  encode_protobuf_string(authenticate_message, 1, protocol_data->user_name);
  write_mumble_message(protocol_data, MUMBLE_AUTHENTICATE, authenticate_message);

  write_mumble_message(protocol_data, MUMBLE_PING, g_byte_array_new());

  purple_connection_set_state(purple_connection, PURPLE_CONNECTION_CONNECTED);
}

static void on_read(GObject *source, GAsyncResult *result, gpointer data) {
  PurpleConnection *connection = data;
  MumbleProtocolData *protocol_data = purple_connection_get_protocol_data(connection);

  if (!protocol_data) {
    return;
  }

  GError *error = NULL;
  MumbleMessage *message = mumble_input_stream_read_message_finish(protocol_data->input_stream, result, &error);
  if (error) {
    purple_connection_take_error(connection, error);
    return;
  }

  switch (message->type) {
    case MUMBLE_CHANNEL_STATE: {
      GByteArray *payload = message->payload;

      guint64 channel_id = 0;
      guint64 parent = 0;
      gchar *name = NULL;
      gchar *description = NULL;
      GArray *links_remove = g_array_new(FALSE, FALSE, sizeof(guint64));
      for (guint offset = 0; offset < payload->len;) {
        guint field_number;
        guint wire_type;
        if (!decode_protobuf_tag(payload, &offset, &field_number, &wire_type)) {
          break;
        }
        switch (field_number) {
          case 1:
            decode_protobuf_unsigned_varint(payload, &offset, &channel_id);
            break;
          case 2:
            decode_protobuf_unsigned_varint(payload, &offset, &parent);
            break;
          case 3:
            decode_protobuf_string(payload, &offset, &name);
            break;
          case 5:
            decode_protobuf_string(payload, &offset, &description);
            break;
          case 7:
            remember_protobuf_unsigned_varint(payload, &offset, links_remove);
            break;
          default:
            skip_protobuf_value(payload, &offset, wire_type);
            break;
        }
      }

      MumbleChannel *channel = mumble_channel_tree_get_channel(protocol_data->tree, channel_id);
      if (channel) {
        if (name) {
          mumble_channel_set_name(channel, name);
        }
        for (int index = 0; index < links_remove->len; index++) {
          mumble_channel_tree_remove_subtree(protocol_data->tree, g_array_index(links_remove, guint64, index));
        }
      } else {
        channel = mumble_channel_new(channel_id, name, description);
        mumble_channel_tree_add_channel(protocol_data->tree, channel, parent);
      }

      g_free(name);
      g_free(description);
      g_array_free(links_remove, TRUE);
      break;
    }
    case MUMBLE_USER_REMOVE: {
      GByteArray *payload = message->payload;

      guint64 session = 0;
      for (guint offset = 0; offset < payload->len;) {
        guint field_number;
        guint wire_type;
        if (!decode_protobuf_tag(payload, &offset, &field_number, &wire_type)) {
          break;
        }
        switch (field_number) {
          case 1:
            decode_protobuf_unsigned_varint(payload, &offset, &session);
            break;
          default:
            skip_protobuf_value(payload, &offset, wire_type);
            break;
        }
      }

      MumbleUser *user = mumble_channel_tree_get_user(protocol_data->tree, session);
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
      GByteArray *payload = message->payload;

      guint64 session = 0;
      gboolean has_channel_id = FALSE;
      guint64 channel_id = 0;
      gchar *name = NULL;
      for (guint offset = 0; offset < payload->len;) {
        guint field_number;
        guint wire_type;
        if (!decode_protobuf_tag(payload, &offset, &field_number, &wire_type)) {
          break;
        }
        switch (field_number) {
          case 1:
            decode_protobuf_unsigned_varint(payload, &offset, &session);
            break;
          case 3:
            decode_protobuf_string(payload, &offset, &name);
            break;
          case 5:
            decode_protobuf_unsigned_varint(payload, &offset, &channel_id);
            has_channel_id = TRUE;
            break;
          default:
            skip_protobuf_value(payload, &offset, wire_type);
            break;
        }
      }

      MumbleUser *user = mumble_channel_tree_get_user(protocol_data->tree, session);
      if (user) {
        if (has_channel_id && (channel_id != user->channel_id)) {
          if (session == protocol_data->session_id) {
            join_channel(connection, mumble_channel_tree_get_channel(protocol_data->tree, channel_id));
          } else {
            if (protocol_data->active_chat) {
              guint active_channel_id = mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id);
              if (user->channel_id == active_channel_id) {
                purple_chat_conversation_remove_user(protocol_data->active_chat, user->name, NULL);
              } else if (channel_id == active_channel_id) {
                purple_chat_conversation_add_user(protocol_data->active_chat, user->name, NULL, 0, FALSE);
              }
            }
          }
          
          user->channel_id = channel_id;
        }
      } else {
        user = mumble_user_new(session, name, channel_id);
        mumble_channel_tree_add_user(protocol_data->tree, user);

        if (!g_strcmp0(protocol_data->user_name, user->name)) {
          protocol_data->session_id = user->session_id;
        }

        if (protocol_data->active_chat) {
          if (user->channel_id == mumble_channel_tree_get_user_channel_id(protocol_data->tree, protocol_data->session_id)) {
            purple_chat_conversation_add_user(protocol_data->active_chat, user->name, NULL, 0, FALSE);
          }
        }
      }

      g_free(name);
      break;
    }
    case MUMBLE_TEXT_MESSAGE: {
      GByteArray *payload = message->payload;

      guint64 actor_value = 0;
      gchar *text_message = NULL;
      for (guint offset = 0; offset < payload->len;) {
        guint field_number;
        guint wire_type;
        if (!decode_protobuf_tag(payload, &offset, &field_number, &wire_type)) {
          break;
        }
        switch (field_number) {
          case 1:
            decode_protobuf_unsigned_varint(payload, &offset, &actor_value);
            break;
          case 5:
            decode_protobuf_string(payload, &offset, &text_message);
            break;
          default:
            skip_protobuf_value(payload, &offset, wire_type);
            break;
        }
      }

      MumbleUser *actor = mumble_channel_tree_get_user(protocol_data->tree, actor_value);
      purple_serv_got_chat_in(connection, purple_chat_conversation_get_id(protocol_data->active_chat), actor->name, 0, text_message, time(NULL));

      g_free(text_message);
      break;
    }
    default: {
      purple_debug_info("mumble", "Read message of type %d", message->type);
      GString *protobuf_debug_info = g_string_new(NULL);
      append_protobuf_debug_info(protobuf_debug_info, message->payload);
      purple_debug_info("mumble", protobuf_debug_info->str);
      g_string_free(protobuf_debug_info, TRUE);
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

      GByteArray *user_state_message = g_byte_array_new();
      encode_protobuf_unsigned_varint(user_state_message, 1, protocol_data->session_id);
      encode_protobuf_unsigned_varint(user_state_message, 5, channel->id);
      write_mumble_message(protocol_data, MUMBLE_USER_STATE, user_state_message);
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

static void write_mumble_message(MumbleProtocolData *protocol_data, MumbleMessageType type, GByteArray *payload) {
  MumbleMessage *message = mumble_message_new(type, payload);
  mumble_output_stream_write_message_async(protocol_data->output_stream, message, protocol_data->cancellable, NULL, NULL);
}
