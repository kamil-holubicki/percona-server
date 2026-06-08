/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

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

#ifndef RPL_BINLOG_SERVER_INCLUDED
#define RPL_BINLOG_SERVER_INCLUDED

class Master_info;

/**
  Validate the generic shape of a binlog server storage URI:
  scheme "://" rest, where scheme is [A-Za-z][A-Za-z0-9+\-.]*
  and rest is at least one byte.

  This is a shape check only -- scheme policy (file://, s3://) is the
  storage component's responsibility.

  @param uri  NUL-terminated string to validate; nullptr returns false.
  @return     true if the URI matches the expected shape.
*/
bool binlog_server_storage_uri_shape_ok(const char *uri);

/**
  Notify the binlog_server component that a channel's BINLOG_SERVER
  configuration changed. Called after CHANGE REPLICATION SOURCE.

  @param mi  Master_info whose binlog_server fields were just updated.
*/
void binlog_server_notify_channel_config(Master_info *mi);

/**
  Notify by channel name — used on START REPLICA IO_THREAD to announce
  a persisted BINLOG_SERVER=1 channel to the component.

  @param channel_name  Replication channel name.
*/
void binlog_server_notify_channel_config_by_name(const char *channel_name);

/**
  Re-announce every BINLOG_SERVER-enabled channel to the component.
  Called from binlog_server_relay plugin init to recover channels that
  were configured before the plugin was loaded.

  @return  Number of channels successfully configured.
*/
unsigned int binlog_server_reconfigure_all_channels();

#endif /* RPL_BINLOG_SERVER_INCLUDED */
