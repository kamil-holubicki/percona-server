/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_LOG_HELPERS_H
#define BINLOG_SERVER_LOG_HELPERS_H

#include <my_compiler.h>
#include <mysql/components/services/log_shared.h>

namespace binlog_server {

void bslog(loglevel level, const char *format, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

void bslog_code(loglevel level, unsigned int errcode, const char *format, ...)
    MY_ATTRIBUTE((format(printf, 3, 4)));

}  // namespace binlog_server

#endif /* BINLOG_SERVER_LOG_HELPERS_H */
