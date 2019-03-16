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

#ifndef MUMBLE_CHANNEL_H
#define MUMBLE_CHANNEL_H

#include <glib-object.h>

typedef struct _MumbleChannel {
  guint id;
  gchar *name;
  gchar *description;
} MumbleChannel;

void mumble_channel_set_description(MumbleChannel *channel, gchar *description);
void mumble_channel_set_name(MumbleChannel *channel, gchar *name);
void mumble_channel_free(MumbleChannel *channel);
MumbleChannel *mumble_channel_copy(MumbleChannel *channel);
MumbleChannel *mumble_channel_new(guint channelId, gchar *name, gchar *description);
GType mumble_channel_get_type();

#endif
