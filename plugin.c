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

#include <purple.h>
#include "mumble-protocol.h"

static PurplePluginInfo *plugin_query(GError **);
static gboolean plugin_load(PurplePlugin *, GError **);
static gboolean plugin_unload(PurplePlugin *, GError **);

static PurpleProtocol *purple_protocol;

gchar *PROTOCOL_ID = "prpl-mumble";

PURPLE_PLUGIN_INIT(mumble, plugin_query, plugin_load, plugin_unload);

static PurplePluginInfo *plugin_query(GError **error) {
  return purple_plugin_info_new(
    "id",          PROTOCOL_ID,
    "abi-version", PURPLE_ABI_VERSION,
    "name",        "Mumble protocol",
    "version",     "0.0.1",
    "category",    "Protocol",
    "summary",     "Mumble protocol plugin",
    "description", "Mumble protocol plugin that supports only text.",
    "license-id",  "GPL",
    "flags",       PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD,
    NULL
  );
}

static gboolean plugin_load(PurplePlugin *plugin, GError **error) {
  mumble_protocol_register(plugin);
  
  purple_protocol = purple_protocols_add(MUMBLE_TYPE_PROTOCOL, error);
  if (!purple_protocol) {
    return FALSE;
  }
  
  return TRUE;
}

static gboolean plugin_unload(PurplePlugin *plugin, GError **error) {
  if (!purple_protocols_remove(purple_protocol, error)) {
    return FALSE;
  }
  
  return TRUE;
}
