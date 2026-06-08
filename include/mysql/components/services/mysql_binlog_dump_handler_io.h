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

#ifndef COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_IO_H
#define COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_IO_H

#include "mysql/components/service.h"
#include "mysql/components/services/bits/thd.h"  // MYSQL_THD

/**
  Service: wire IO for a dump session intercepted by the binlog server.

  send_event ships raw event bytes (OK byte prepended internally).
  flush ensures pending bytes reach the replica.
*/
BEGIN_SERVICE_DEFINITION(mysql_binlog_dump_handler_io)

/**
  Send a single binlog event to the connecting replica.
  The dump-protocol OK byte is prepended internally.
*/
DECLARE_BOOL_METHOD(send_event, (MYSQL_THD thd, const unsigned char *event_buf,
                                 unsigned long event_len));

/**
  Flush pending bytes on the dump connection.
*/
DECLARE_BOOL_METHOD(flush, (MYSQL_THD thd));

END_SERVICE_DEFINITION(mysql_binlog_dump_handler_io)

#endif /* COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_IO_H */
