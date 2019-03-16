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

#ifndef MUMBLE_USER_H
#define MUMBLE_USER_H

#include <glib-object.h>

typedef struct _MumbleUser {
  guint sessionId;
  gchar *name;
  guint channelId;
} MumbleUser;

void mumble_user_set_name(MumbleUser *user, gchar *name);
void mumble_user_free(MumbleUser *user);
MumbleUser *mumble_user_copy(MumbleUser *user);
MumbleUser *mumble_user_new(guint sessionId, gchar *name, guint channelId);
GType mumble_user_get_type();

#endif
