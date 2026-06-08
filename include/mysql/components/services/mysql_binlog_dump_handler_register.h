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

#ifndef COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_REGISTER_H
#define COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_REGISTER_H

#include "mysql/components/service.h"
#include "mysql/components/services/bits/mysql_binlog_dump_handler_bits.h"

/**
  Service: register / unregister the process-wide binlog dump handler.

  At most one handler is installed at a time. Installation is process-wide
  and uses acquire/release atomics.
*/
BEGIN_SERVICE_DEFINITION(mysql_binlog_dump_handler_register)

/**
  Install handler as the process-wide dump handler.
  @retval false Installed.
  @retval true  A handler is already installed.
*/
DECLARE_BOOL_METHOD(install, (mysql_binlog_dump_handler_fn handler,
                              void *user_data));

/**
  Uninstall the currently-installed dump handler.
  @retval false Always.
*/
DECLARE_BOOL_METHOD(uninstall, ());

END_SERVICE_DEFINITION(mysql_binlog_dump_handler_register)

#endif /* COMPONENTS_SERVICES_MYSQL_BINLOG_DUMP_HANDLER_REGISTER_H */
