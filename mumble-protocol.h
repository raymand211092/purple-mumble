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

#ifndef MUMBLE_PROTOCOL_H
#define MUMBLE_PROTOCOL_H

#include <purple.h>

/**
 * SECTION:mumbleprotocol
 * @short_description: Mumble protocol
 *
 * Mumble protocol that attaches to the libpurple protocols subsystem.
 */

#define MUMBLE_TYPE_PROTOCOL            mumble_protocol_get_type()
#define MUMBLE_PROTOCOL(obj)            G_TYPE_CHECK_INSTANCE_CAST((obj), MUMBLE_TYPE_PROTOCOL, MumbleProtocol)
#define MUMBLE_PROTOCOL_CLASS(klass)    G_TYPE_CHECK_CLASS_CAST((klass), MUMBLE_TYPE_PROTOCOL, MumbleProtocolClass)
#define MUMBLE_IS_PROTOCOL(obj)         G_TYPE_CHECK_INSTANCE_TYPE((obj), MUMBLE_TYPE_PROTOCOL)
#define MUMBLE_IS_PROTOCOL_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE((klass), MUMBLE_TYPE_PROTOCOL)
#define MUMBLE_PROTOCOL_GET_CLASS(obj)  G_TYPE_INSTANCE_GET_CLASS((obj), MUMBLE_TYPE_PROTOCOL, MumbleProtocolClass)

typedef struct _MumbleProtocol {
  PurpleProtocol parent;
} MumbleProtocol;

typedef struct _MumbleProtocolClass {
  PurpleProtocolClass parent;
} MumbleProtocolClass;

GType mumble_protocol_get_type();
void mumble_protocol_register(PurplePlugin *);

#endif
