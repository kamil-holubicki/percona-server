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

#ifndef COMPONENTS_SERVICES_BITS_MYSQL_BINLOG_DUMP_HANDLER_BITS_H
#define COMPONENTS_SERVICES_BITS_MYSQL_BINLOG_DUMP_HANDLER_BITS_H

#include <cstdint>

#include "mysql/components/services/bits/thd.h"  // MYSQL_THD

/**
  Replica-negotiated binlog checksum algorithm.
*/
enum mysql_binlog_dump_handler_checksum_alg : int16_t {
  MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_UNDEF = -1,
  MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_OFF = 0,
  MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_CRC32 = 1,
};

/**
  Dispatch callback signature installed by the component via
  mysql_binlog_dump_handler_register::install.

  Invoked from mysql_binlog_send() before the default Binlog_sender.
  Returns true if the dump session is fully serviced; false to fall through.

  All parameters except thd are value snapshots. The component must NOT
  retain pointers past return. Pass thd only to _io service methods.
*/
typedef bool (*mysql_binlog_dump_handler_fn)(
    void *user_data, MYSQL_THD thd, const char *log_ident, uint64_t pos,
    const char *replica_executed_gtids_text, uint32_t flags,
    uint32_t source_server_id, uint32_t replica_server_id,
    enum mysql_binlog_dump_handler_checksum_alg negotiated_checksum_alg,
    const char *replica_user);

#endif /* COMPONENTS_SERVICES_BITS_MYSQL_BINLOG_DUMP_HANDLER_BITS_H */
