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

#ifndef MUMBLE_CHANNEL_TREE_H
#define MUMBLE_CHANNEL_TREE_H

#include <glib-object.h>
#include "mumble-channel.h"
#include "mumble-user.h"

typedef struct _MumbleChannelTree {
  GHashTable *idToChannel;
  GHashTable *idToUser;
  GNode *root;
} MumbleChannelTree;

void mumble_channel_tree_set_user_channel_id(MumbleChannelTree *tree, guint sessionId, guint channelId);
guint mumble_channel_tree_get_user_channel_id(MumbleChannelTree *tree, guint sessionId);
MumbleChannel *mumble_channel_tree_get_channel_by_name(MumbleChannelTree *tree, gchar *name);
GList *mumble_channel_tree_get_channel_user_names(MumbleChannelTree *tree, guint channelId);
void mumble_channel_tree_remove_user(MumbleChannelTree *tree, guint sessionId);
void mumble_channel_tree_add_user(MumbleChannelTree *tree, MumbleUser *user);
MumbleUser *mumble_channel_tree_get_user(MumbleChannelTree *tree, guint sessionId);
void mumble_channel_tree_add_channel(MumbleChannelTree *tree, MumbleChannel *channel, guint parentId);
void mumble_channel_tree_remove_subtree(MumbleChannelTree *tree, guint channelId);
MumbleChannel *mumble_channel_tree_get_channel(MumbleChannelTree *tree, guint channelId);
void mumble_channel_tree_free(MumbleChannelTree *tree);
MumbleChannelTree *mumble_channel_tree_copy(MumbleChannelTree *tree);
MumbleChannelTree *mumble_channel_tree_new();
GType mumble_channel_tree_get_type();

#endif
