/*
 * purple-mumble -- Mumble protocol plugin for libpurple
 * Copyright (C) 2020  Petteri Pitkänen
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

#include "protobuf-utils.h"

static void encode_tag(GByteArray *message, guint field_number, guint wire_type);
static void encode_varint(GByteArray *message, guint64 value);

void append_protobuf_debug_info(GString *string, GByteArray *message) {
  for (guint offset = 0; offset < message->len;) {
    guint field_number;
    guint wire_type;
    if (!decode_protobuf_tag(message, &offset, &field_number, &wire_type)) {
      return;
    }
    guint begin_offset = offset;
    skip_protobuf_value(message, &offset, wire_type);
    guint end_offset = offset;
    if (end_offset <= message->len) {
      g_string_append_printf(string, "(%d:", field_number);
      for (guint i = begin_offset; i < end_offset; i++) {
        g_string_append_printf(string, "%.2X", message->data[i]);
      }
      g_string_append(string, ")");
    }
  }
}

gboolean remember_protobuf_unsigned_varint(GByteArray *message, guint *offset, GArray *values) {
  guint64 value;
  if (decode_protobuf_unsigned_varint(message, offset, &value)) {
    return FALSE;
  }
  g_array_append_val(values, value);
  return TRUE;
}

void skip_protobuf_value(GByteArray *message, guint *offset, guint wire_type) {
  switch (wire_type) {
    case 0: {
      guint64 value;
      decode_protobuf_unsigned_varint(message, offset, &value); // TODO Optimize for speed.
      break;
    }
    case 1: {
      *offset += 8;
      break;
    }
    case 2: {
      guint64 length;
      decode_protobuf_unsigned_varint(message, offset, &length);
      *offset += length;
      break;
    }
    case 5: {
      *offset += 4;
      break;
    }
  }
}

gboolean decode_protobuf_string(GByteArray *message, guint *offset, gchar **value) {
  guint64 length;
  if (!decode_protobuf_unsigned_varint(message, offset, &length)) {
    return FALSE;
  }
  *value = g_strndup(&message->data[*offset], length);
  *offset += length;
  return TRUE;
}

gboolean decode_protobuf_tag(GByteArray *message, guint *offset, guint *field_number, guint *wire_type) {
  guint64 tag;
  if (!decode_protobuf_unsigned_varint(message, offset, &tag)) {
    return FALSE;
  }
  *field_number = tag >> 3;
  *wire_type = tag & 7;
  return TRUE;
}

gboolean decode_protobuf_unsigned_varint(GByteArray *message, guint *offset, guint64 *value) {
  *value = 0;
  while (message->data[*offset] >= 0x80) {
    if ((*offset) > message->len) {
      return FALSE;
    }
    *value = ((*value) << 7) | (message->data[(*offset)++] & 0x7F);
  }
  if ((*offset) >= message->len) {
    return FALSE;
  }
  *value = ((*value) << 7) | message->data[(*offset)++];
  return TRUE;
}

void encode_protobuf_string(GByteArray *message, guint field_number, gchar *value) {
  encode_tag(message, field_number, 2);
  guint64 count = strlen(value);
  encode_varint(message, count);
  g_byte_array_append(message, value, count);
}

void encode_protobuf_unsigned_varint(GByteArray *message, guint field_number, guint64 value) {
  encode_tag(message, field_number, 0);
  encode_varint(message, value);
}

static void encode_tag(GByteArray *message, guint field_number, guint wire_type) {
  encode_varint(message, (field_number << 3) | wire_type);
}

static void encode_varint(GByteArray *message, guint64 value) {
  guint8 byte;
  while (value >= 0x80) {
    byte = 0x80 | (value & 0x7F);
    g_byte_array_append(message, &byte, 1);
    value >>= 7;
  }
  byte = value & 0x7F;
  g_byte_array_append(message, &byte, 1);
}
