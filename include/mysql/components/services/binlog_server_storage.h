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

#ifndef BINLOG_SERVER_STORAGE_SERVICE_HEADERS_H
#define BINLOG_SERVER_STORAGE_SERVICE_HEADERS_H

#include <mysql/components/service.h>

BEGIN_SERVICE_DEFINITION(binlog_server_storage)

/**
  Update per-channel binlog server configuration.

  @param channel_name replication channel name
  @param enabled      non-zero if BINLOG_SERVER is enabled for the channel
  @param storage_uri  per-channel storage URI, or nullptr/empty to use default
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, configure_channel,
               (const char *channel_name, int enabled,
                const char *storage_uri));

/**
  Append a replication event to the binlog server storage for a channel.

  @param channel_name replication channel name
  @param event_buf    serialized event bytes
  @param event_len    length of event_buf
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, append_event,
               (const char *channel_name, const char *event_buf,
                unsigned long event_len));

/**
  IO-thread stop hook: flush any buffered bytes for the channel's
  current archive file to the OS and close the file descriptor.

  @param channel_name replication channel name
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, close_channel, (const char *channel_name));

/**
  RESET REPLICA hook: wipe the on-disk archive for a channel and reset
  all per-channel in-memory bookkeeping.

  @param channel_name replication channel name
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, reset_channel, (const char *channel_name));

/**
  Force-flush any buffered data for the channel to the OS.

  @param channel_name replication channel name
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, flush_channel, (const char *channel_name));

/**
  External rotation hint: close the channel's current archive file and
  switch its in-memory state to a new log name.

  @param channel_name  replication channel name
  @param new_log_name  basename of the new source binlog file
  @return 0 on success, non-zero on failure
*/
DECLARE_METHOD(int, rotate_channel,
               (const char *channel_name, const char *new_log_name));

END_SERVICE_DEFINITION(binlog_server_storage);

#endif /* BINLOG_SERVER_STORAGE_SERVICE_HEADERS_H */
