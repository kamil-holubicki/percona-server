/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql_executor.h"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_command_services.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysqld_error.h>

#include <cstring>

extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_factory,
                                       rest_command_factory_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_options,
                                       rest_command_options_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query,
                                       rest_command_query_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query_result,
                                       rest_command_query_result_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_field_info,
                                       rest_command_field_info_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_error_info,
                                       rest_command_error_info_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_thread,
                                       rest_command_thread_srv);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_command_field_metadata);

namespace rest_api {

namespace {
thread_local bool tl_thread_initialized = false;

void ensure_thread_init() {
  if (!tl_thread_initialized) {
    rest_command_thread_srv->init();
    tl_thread_initialized = true;
  }
}
}  // namespace

SqlResult execute_query(const std::string &sql) {
  ensure_thread_init();

  SqlResult result;
  MYSQL_H mysql_h = nullptr;
  MYSQL_RES_H mysql_res = nullptr;

  auto close_handle = [&]() {
    if (mysql_h != nullptr) {
      rest_command_factory_srv->close(mysql_h);
      mysql_h = nullptr;
    }
  };
  auto free_res = [&]() {
    if (mysql_res != nullptr) {
      rest_command_query_result_srv->free_result(mysql_res);
      mysql_res = nullptr;
    }
  };

  if (rest_command_factory_srv->init(&mysql_h)) {
    result.error = "mysql_command_factory.init failed";
    return result;
  }
  if (rest_command_options_srv->set(mysql_h, MYSQL_COMMAND_PROTOCOL, nullptr) ||
      rest_command_options_srv->set(mysql_h, MYSQL_COMMAND_USER_NAME,
                                   "mysql.session") ||
      rest_command_options_srv->set(mysql_h, MYSQL_COMMAND_HOST_NAME,
                                   "localhost")) {
    result.error = "mysql_command_options.set failed";
    close_handle();
    return result;
  }
  if (rest_command_factory_srv->connect(mysql_h)) {
    result.error = "connect as 'mysql.session'@'localhost' failed";
    close_handle();
    return result;
  }
  if (rest_command_query_srv->query(mysql_h, sql.data(), sql.length())) {
    char err_buf[512];
    err_buf[0] = '\0';
    char *err_ptr = err_buf;
    if (!rest_command_error_info_srv->sql_error(mysql_h, &err_ptr) &&
        err_buf[0] != '\0') {
      result.error = std::string("query failed: ") + err_buf;
    } else {
      result.error = "query failed";
    }
    close_handle();
    return result;
  }
  if (rest_command_query_result_srv->store_result(mysql_h, &mysql_res)) {
    result.error = "store_result failed";
    close_handle();
    return result;
  }
  if (mysql_res == nullptr) {
    result.ok = true;
    close_handle();
    return result;
  }

  unsigned int num_columns = 0;
  if (rest_command_field_info_srv->num_fields(mysql_res, &num_columns)) {
    result.error = "num_fields failed";
    free_res();
    close_handle();
    return result;
  }

  // Get column names via fetch_field + field_metadata
  std::vector<std::string> column_names;
  column_names.reserve(num_columns);
  for (unsigned int i = 0; i < num_columns; ++i) {
    MYSQL_FIELD_H field_h = nullptr;
    if (!rest_command_field_info_srv->fetch_field(mysql_res, &field_h) &&
        field_h != nullptr) {
      const char *name = nullptr;
      if (!mysql_service_mysql_command_field_metadata->get(
              field_h, MYSQL_COMMAND_FIELD_METADATA_NAME, &name) &&
          name != nullptr) {
        column_names.emplace_back(name);
      } else {
        column_names.emplace_back("col_" + std::to_string(i));
      }
    } else {
      column_names.emplace_back("col_" + std::to_string(i));
    }
  }

  uint64_t row_count = 0;
  rest_command_query_srv->affected_rows(mysql_h, &row_count);

  for (uint64_t r = 0; r < row_count; ++r) {
    MYSQL_ROW_H row = nullptr;
    if (rest_command_query_result_srv->fetch_row(mysql_res, &row)) break;
    if (row == nullptr) break;

    ResultRow rr;
    rr.columns = column_names;
    rr.values.reserve(num_columns);
    for (unsigned int c = 0; c < num_columns; ++c) {
      const char *val = row[c];
      rr.values.emplace_back(val != nullptr ? val : "");
    }
    result.rows.push_back(std::move(rr));
  }

  free_res();
  close_handle();
  result.ok = true;
  return result;
}

}  // namespace rest_api
