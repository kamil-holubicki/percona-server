/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "log_helpers.h"

#include <cstdarg>
#include <cstdio>

#include <mysql/components/component_implementation.h>

#define LOG_COMPONENT_TAG "binlog_server"

#include <mysql/components/services/log_builtins.h>
#include "mysqld_error.h"

extern SERVICE_TYPE(log_builtins) *log_bi;
extern SERVICE_TYPE(log_builtins_string) *log_bs;

namespace binlog_server {

void bslog(loglevel level, const char *format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  LogComponentErr(level, ER_LOG_PRINTF_MSG, buf);
}

void bslog_code(loglevel level, unsigned int errcode, const char *format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  LogComponentErr(level, errcode, buf);
}

}  // namespace binlog_server
