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

#include "mumble-user.h"

void mumble_user_set_name(MumbleUser *user, gchar *name) {
  g_free(user->name);
  user->name = g_strdup(name);
}

void mumble_user_free(MumbleUser *user) {
  g_free(user->name);
  g_free(user);
}

MumbleUser *mumble_user_copy(MumbleUser *user) {
  MumbleUser *copy = mumble_user_new(user->session_id, user->name, user->channel_id);
  return copy;
}

MumbleUser *mumble_user_new(guint session_id, gchar *name, guint channel_id) {
  MumbleUser *user = g_new0(MumbleUser, 1);

  user->session_id = session_id;
  mumble_user_set_name(user, name);
  user->channel_id = channel_id;

  return user;
}

G_DEFINE_BOXED_TYPE(MumbleUser, mumble_user, mumble_user_copy, mumble_user_free)
