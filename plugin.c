/*
 * purple-mumble -- Mumble protocol plugin for libpurple
 * Copyright (C) 2018  Petteri Pitkänen
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
#include <glib/gi18n.h>
#include "mumble-protocol.h"

static PurplePluginInfo *plugin_query(GError **);
static gboolean plugin_load(PurplePlugin *, GError **);
static gboolean plugin_unload(PurplePlugin *, GError **);

PURPLE_PLUGIN_INIT(mumble, plugin_query, plugin_load, plugin_unload);

static PurpleProtocol *purpleProtocol;

static PurplePluginInfo *plugin_query(GError **error) {
  return purple_plugin_info_new(
    "id",          "prpl-mumble",
    "abi-version", PURPLE_ABI_VERSION,
    "name",        "Mumble protocol",
    "version",     "0.0.1",
    "category",    N_("Protocol"),
    "summary",     N_("Mumble protocol plugin"),
    "description", N_("Mumble protocol plugin that supports only text."),
    "license-id",  "GPL",
    "flags",       PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD,
    NULL
  );
}

static gboolean plugin_load(PurplePlugin *plugin, GError **error) {
  fprintf(stderr, "plugin_load()\n");
  
  mumble_protocol_register_type(plugin);
  
  purpleProtocol = purple_protocols_add(MUMBLE_TYPE_PROTOCOL, error);
  if (!purpleProtocol) {
    return FALSE;
  }
  
  return TRUE;
}

static gboolean plugin_unload(PurplePlugin *plugin, GError **error) {
  if (!purple_protocols_remove(purpleProtocol, error)) {
    return FALSE;
  }
  
  return TRUE;
}
