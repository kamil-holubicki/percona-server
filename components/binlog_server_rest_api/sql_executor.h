/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_REST_API_SQL_EXECUTOR_H
#define BINLOG_SERVER_REST_API_SQL_EXECUTOR_H

#include <functional>
#include <string>
#include <vector>

namespace rest_api {

struct ResultRow {
  std::vector<std::string> columns;
  std::vector<std::string> values;
};

using RowCallback = std::function<void(const ResultRow &)>;

struct SqlResult {
  bool ok{false};
  std::string error;
  std::vector<ResultRow> rows;
};

SqlResult execute_query(const std::string &sql);

}  // namespace rest_api

#endif
