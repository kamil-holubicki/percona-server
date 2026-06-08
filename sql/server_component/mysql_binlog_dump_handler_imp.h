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

#ifndef SQL_SERVER_COMPONENT_MYSQL_BINLOG_DUMP_HANDLER_IMP_H
#define SQL_SERVER_COMPONENT_MYSQL_BINLOG_DUMP_HANDLER_IMP_H

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_binlog_dump_handler_io.h>
#include <mysql/components/services/mysql_binlog_dump_handler_register.h>

class mysql_binlog_dump_handler_register_imp {
 public:
  static DEFINE_BOOL_METHOD(install,
                            (mysql_binlog_dump_handler_fn handler,
                             void *user_data));

  static DEFINE_BOOL_METHOD(uninstall, ());
};

class mysql_binlog_dump_handler_io_imp {
 public:
  static DEFINE_BOOL_METHOD(send_event,
                            (MYSQL_THD thd, const unsigned char *event_buf,
                             unsigned long event_len));
  static DEFINE_BOOL_METHOD(flush, (MYSQL_THD thd));
};

#endif /* SQL_SERVER_COMPONENT_MYSQL_BINLOG_DUMP_HANDLER_IMP_H */
