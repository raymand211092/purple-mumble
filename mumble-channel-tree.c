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

#include "mumble-channel-tree.h"
#include "mumble-user.h"
#include "utils.h"

static GNode *mumble_channel_tree_get_node(MumbleChannelTree *tree, guint channelId);

guint mumble_channel_tree_get_parent_id(MumbleChannelTree *tree, guint channelId) {
  GNode *node = mumble_channel_tree_get_node(tree, channelId);

  guint parentId = -1;
  if (node->parent) {
    parentId = ((MumbleChannel *) node->parent->data)->id;
  }

  return parentId;
}

GList *mumble_channel_tree_get_channels_in_topological_order(MumbleChannelTree *tree) {
  GList *channels = NULL;
  g_node_traverse(tree->root, G_PRE_ORDER, G_TRAVERSE_ALL, -1, g_node_traverse_func_create_list, &channels);
  return channels;
}

void mumble_channel_tree_set_user_channel_id(MumbleChannelTree *tree, guint sessionId, guint channelId) {
  mumble_channel_tree_get_user(tree, sessionId)->channelId = channelId;
}

guint mumble_channel_tree_get_user_channel_id(MumbleChannelTree *tree, guint sessionId) {
  MumbleUser *user = mumble_channel_tree_get_user(tree, sessionId);
  return user ? user->channelId : -1;
}

MumbleChannel *mumble_channel_tree_get_channel_by_name(MumbleChannelTree *tree, gchar *name) {
  MumbleChannel *channel = NULL;
  GList *channels = g_hash_table_get_values(tree->idToChannel);
  for (GList *node = channels; node; node = node->next) {
    MumbleChannel *cursorChannel = node->data;
    if (!g_strcmp0(name, cursorChannel->name)) {
      channel = cursorChannel;
      break;
    }
  }
  g_list_free(channels);
  return channel;
}

GList *mumble_channel_tree_get_channel_user_names(MumbleChannelTree *tree, guint channelId) {
  GList *names = NULL;
  GList *users = g_hash_table_get_values(tree->idToUser);
  for (GList *node = users; node; node = node->next) {
    MumbleUser *user = node->data;
    if (user->channelId == channelId) {
      names = g_list_append(names, user->name);
    }
  }

  g_list_free(users);

  return names;
}

void mumble_channel_tree_remove_user(MumbleChannelTree *tree, guint sessionId) {
  g_hash_table_remove(tree->idToUser, &sessionId);
}

void mumble_channel_tree_add_user(MumbleChannelTree *tree, MumbleUser *user) {
  g_hash_table_insert(tree->idToUser, &user->sessionId, user);
}

MumbleUser *mumble_channel_tree_get_user(MumbleChannelTree *tree, guint sessionId) {
  return g_hash_table_lookup(tree->idToUser, &sessionId);
}

void mumble_channel_tree_add_channel(MumbleChannelTree *tree, MumbleChannel *channel, guint parentId) {
  GNode *parent = mumble_channel_tree_get_node(tree, parentId);
  if (parent) {
    g_node_append(parent, g_node_new(channel));
    g_hash_table_insert(tree->idToChannel, &channel->id, channel);
  }
}

void mumble_channel_tree_remove_subtree(MumbleChannelTree *tree, guint channelId) {
  GNode *subtree = mumble_channel_tree_get_node(tree, channelId);
  if (subtree) {
    g_node_destroy(subtree);
  }
}

static GNode *mumble_channel_tree_get_node(MumbleChannelTree *tree, guint channelId) {
  return g_node_find(tree->root, G_IN_ORDER, G_TRAVERSE_ALL, mumble_channel_tree_get_channel(tree, channelId));
}

MumbleChannel *mumble_channel_tree_get_channel(MumbleChannelTree *tree, guint channelId) {
  return g_hash_table_lookup(tree->idToChannel, &channelId);
}

void mumble_channel_tree_free(MumbleChannelTree *tree) {
  g_hash_table_destroy(tree->idToChannel);
  g_hash_table_destroy(tree->idToUser);
  g_node_destroy(tree->root);
}

MumbleChannelTree *mumble_channel_tree_copy(MumbleChannelTree *tree) {
  return NULL;
}

MumbleChannelTree *mumble_channel_tree_new() {
  MumbleChannelTree *tree = g_new0(MumbleChannelTree, 1);

  tree->idToChannel = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, mumble_channel_free);
  tree->idToUser    = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, mumble_user_free);

  // There is always a root channel.
  MumbleChannel *channel = mumble_channel_new(0, "Root", "");
  tree->root = g_node_new(channel);
  g_hash_table_insert(tree->idToChannel, &channel->id, channel);

  return tree;
}

G_DEFINE_BOXED_TYPE(MumbleChannelTree, mumble_channel_tree, mumble_channel_tree_copy, mumble_channel_tree_free)
