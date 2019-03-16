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

#include "mumble-channel.h"

void mumble_channel_set_description(MumbleChannel *channel, gchar *description) {
  g_free(channel->description);
  channel->description = g_strdup(description);
}

void mumble_channel_set_name(MumbleChannel *channel, gchar *name) {
  g_free(channel->name);
  channel->name = g_strdup(name);
}

void mumble_channel_free(MumbleChannel *channel) {
  g_free(channel->name);
  g_free(channel);
}

MumbleChannel *mumble_channel_copy(MumbleChannel *channel) {
  MumbleChannel *copy = mumble_channel_new(channel->id, channel->name, channel->description);
  return copy;
}

MumbleChannel *mumble_channel_new(guint32 channelId, gchar *name, gchar *description) {
  MumbleChannel *channel = g_new0(MumbleChannel, 1);

  channel->id = channelId;
  mumble_channel_set_name(channel, name);
  mumble_channel_set_description(channel, description);

  return channel;
}

G_DEFINE_BOXED_TYPE(MumbleChannel, mumble_channel, mumble_channel_copy, mumble_channel_free)
