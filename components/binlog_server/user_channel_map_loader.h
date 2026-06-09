/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef COMPONENTS_BINLOG_SERVER_USER_CHANNEL_MAP_LOADER_H
#define COMPONENTS_BINLOG_SERVER_USER_CHANNEL_MAP_LOADER_H

#include <string>

/*
  user_channel_map_loader

  Helpers that parse the binlog_server.user_channel_map spec and load
  the live UserChannelMap from one of two sources:

    1. Inline CSV, e.g. "alice=upstream,bob=other"
    2. A server-local table, addressed as "table://<db>.<tbl>" with two
       columns named 'user_name' (VARCHAR) and 'channel_name' (VARCHAR).
       Extra columns are ignored. Other identifier shapes are rejected
       at SET GLOBAL time so we never construct SQL that needs quoting.

  For (2) we open a transient session via the mysql_command_* component
  services as 'mysql.session'@'localhost' (the built-in service user) and
  issue:

      SELECT user_name, channel_name FROM <db>.<tbl>

  Operators must grant SELECT to 'mysql.session'@'localhost' on the
  mapping table before the read can succeed:

      GRANT SELECT ON <db>.<tbl> TO 'mysql.session'@'localhost';
*/

namespace binlog_server {
namespace user_channel_map_loader {

bool looks_like_table_uri(const char *spec);

/*
  Strict validation of a "table://<db>.<tbl>" URI. Identifiers must
  match [A-Za-z0-9_$]+ so we can splice them into SQL without quoting.
  On success db / tbl are filled; on failure they are left unchanged
  and err_msg (if not nullptr) is set to a human-readable reason.
*/
bool parse_table_uri(const char *spec, std::string *db, std::string *tbl,
                     std::string *err_msg);

/*
  Apply the given spec to the live UserChannelMap held by ArchiveSender.

    spec == nullptr || spec[0] == '\0'    -> clear the map. Returns 0.
    looks_like_table_uri(spec)            -> fetch from the table.
    otherwise                             -> parse CSV.

  Returns the resulting mapping count on success.
  Returns -1 on failure; the LIVE map is preserved and err_msg is set.

  CAUTION: the table:// branch opens an internal Srv_session via
  mysql_command_*; opening that session takes LOCK_plugin. Do NOT call
  from the sysvar update callback (which already holds LOCK_plugin).

  Safe contexts: component_init(), UDF entry.
*/
long long apply_spec(const char *spec, std::string *err_msg);

/*
  Variant safe to call from the sysvar update callback (LOCK_plugin
  held). Identical to apply_spec() except that the table:// branch
  does NOT contact the SQL engine: returns -2 to signal "deferred".

  The clear and CSV branches behave exactly like apply_spec().
*/
long long apply_spec_no_sql(const char *spec, std::string *err_msg);

}  // namespace user_channel_map_loader
}  // namespace binlog_server

#endif  // COMPONENTS_BINLOG_SERVER_USER_CHANNEL_MAP_LOADER_H
