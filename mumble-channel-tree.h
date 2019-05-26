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
  GHashTable *id_to_channel;
  GHashTable *id_to_user;
  GNode *root;
} MumbleChannelTree;

gboolean mumble_channel_tree_has_children(MumbleChannelTree *tree, guint channel_id);
guint mumble_channel_tree_get_parent_id(MumbleChannelTree *tree, guint channel_id);
GList *mumble_channel_tree_get_channels_in_topological_order(MumbleChannelTree *tree);
void mumble_channel_tree_set_user_channel_id(MumbleChannelTree *tree, guint session_id, guint channel_id);
guint mumble_channel_tree_get_user_channel_id(MumbleChannelTree *tree, guint session_id);
MumbleChannel *mumble_channel_tree_get_channel_by_name(MumbleChannelTree *tree, gchar *name);
GList *mumble_channel_tree_get_channel_user_names(MumbleChannelTree *tree, guint channel_id);
void mumble_channel_tree_remove_user(MumbleChannelTree *tree, guint session_id);
void mumble_channel_tree_add_user(MumbleChannelTree *tree, MumbleUser *user);
MumbleUser *mumble_channel_tree_get_user(MumbleChannelTree *tree, guint session_id);
void mumble_channel_tree_add_channel(MumbleChannelTree *tree, MumbleChannel *channel, guint parent_id);
void mumble_channel_tree_remove_subtree(MumbleChannelTree *tree, guint channel_id);
MumbleChannel *mumble_channel_tree_get_channel(MumbleChannelTree *tree, guint channel_id);
void mumble_channel_tree_free(MumbleChannelTree *tree);
MumbleChannelTree *mumble_channel_tree_copy(MumbleChannelTree *tree);
MumbleChannelTree *mumble_channel_tree_new();
GType mumble_channel_tree_get_type();

#endif
