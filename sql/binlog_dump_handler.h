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

#ifndef SQL_BINLOG_DUMP_HANDLER_H_INCLUDED
#define SQL_BINLOG_DUMP_HANDLER_H_INCLUDED

#include "my_inttypes.h"  // my_off_t, uint32

class Gtid_set;
class THD;

/**
  Internal mysqld-only dispatch slot for the binlog server dump handler.

  External components acquire the mysql_binlog_dump_handler_register service
  through the registry. The service bridge in
  sql/server_component/mysql_binlog_dump_handler_imp.cc translates between
  this internal slot and the component-side callback.

  Thread safety: the function pointer uses acquire/release atomics.
*/
using Binlog_dump_handler_func = bool (*)(THD *thd, const char *log_ident,
                                          my_off_t pos, Gtid_set *gtid_set,
                                          uint32 flags);

/// Install a custom dump handler. Pass nullptr to uninstall.
void set_binlog_dump_handler(Binlog_dump_handler_func handler);

/// Read the current dump handler (nullptr if none installed).
Binlog_dump_handler_func get_binlog_dump_handler();

#endif /* SQL_BINLOG_DUMP_HANDLER_H_INCLUDED */
