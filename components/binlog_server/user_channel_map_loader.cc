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

#include "components/binlog_server/user_channel_map_loader.h"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/my_err_bits.h>
#include <mysql/components/services/mysql_command_services.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysqld_error.h>

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "components/binlog_server/archive_sender.h"
#include "components/binlog_server/log_helpers.h"

/*
  Service handles are OWNED by binlog_server_component.cc (see the
  REQUIRES_SERVICE_PLACEHOLDER_AS lines there). They are referenced from
  this translation unit through the placeholder symbols the chassis
  emits -- the linker resolves them from the same shared object.
*/
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_factory,
                                       command_factory_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_options,
                                       command_options_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query, command_query_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query_result,
                                       command_query_result_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_field_info,
                                       command_field_info_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_error_info,
                                       command_error_info_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_current_thread_reader,
                                       current_thread_reader_srv);

namespace binlog_server {
namespace user_channel_map_loader {

namespace {

constexpr char kTableUriPrefix[] = "table://";
constexpr std::size_t kTableUriPrefixLen = sizeof(kTableUriPrefix) - 1;

bool is_bare_identifier(const std::string &s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '_' || c == '$')) return false;
  }
  return true;
}

bool parse_csv(const char *csv,
               std::vector<std::pair<std::string, std::string>> *out,
               std::string *err_msg) {
  if (csv == nullptr) csv = "";
  std::string item;
  std::string buf(csv);
  std::size_t i = 0;
  while (i <= buf.size()) {
    if (i == buf.size() || buf[i] == ',') {
      std::size_t a = 0, b = item.size();
      while (a < b && std::isspace(static_cast<unsigned char>(item[a]))) ++a;
      while (b > a && std::isspace(static_cast<unsigned char>(item[b - 1])))
        --b;
      std::string trimmed = item.substr(a, b - a);
      item.clear();
      if (!trimmed.empty()) {
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos || eq == 0 ||
            eq == trimmed.size() - 1) {
          if (err_msg != nullptr) {
            *err_msg =
                "expected 'user1=channel1,user2=channel2,...' or "
                "'table://<db>.<tbl>'";
          }
          return false;
        }
        std::string u = trimmed.substr(0, eq);
        std::string c = trimmed.substr(eq + 1);
        auto trim = [](std::string &s) {
          std::size_t aa = 0, bb = s.size();
          while (aa < bb &&
                 std::isspace(static_cast<unsigned char>(s[aa])))
            ++aa;
          while (bb > aa &&
                 std::isspace(static_cast<unsigned char>(s[bb - 1])))
            --bb;
          s = s.substr(aa, bb - aa);
        };
        trim(u);
        trim(c);
        if (u.empty() || c.empty()) {
          if (err_msg != nullptr) {
            *err_msg = "empty user or channel name in user_channel_map";
          }
          return false;
        }
        out->emplace_back(std::move(u), std::move(c));
      }
    } else {
      item.push_back(buf[i]);
    }
    ++i;
  }
  return true;
}

bool run_select(const std::string &query,
                std::vector<std::pair<std::string, std::string>> *out,
                std::string *err_msg) {
  out->clear();

  MYSQL_H mysql_h = nullptr;
  MYSQL_RES_H mysql_res = nullptr;

  auto close_handle = [&]() {
    if (mysql_h != nullptr) {
      command_factory_srv->close(mysql_h);
      mysql_h = nullptr;
    }
  };
  auto free_res = [&]() {
    if (mysql_res != nullptr) {
      command_query_result_srv->free_result(mysql_res);
      mysql_res = nullptr;
    }
  };

  if (command_factory_srv->init(&mysql_h)) {
    if (err_msg != nullptr) *err_msg = "mysql_command_factory.init failed";
    close_handle();
    return false;
  }
  if (command_options_srv->set(mysql_h, MYSQL_COMMAND_PROTOCOL, nullptr) ||
      command_options_srv->set(mysql_h, MYSQL_COMMAND_USER_NAME,
                               "mysql.session") ||
      command_options_srv->set(mysql_h, MYSQL_COMMAND_HOST_NAME,
                               "localhost")) {
    if (err_msg != nullptr) *err_msg = "mysql_command_options.set failed";
    close_handle();
    return false;
  }
  if (command_factory_srv->connect(mysql_h)) {
    if (err_msg != nullptr) {
      *err_msg = "connect as 'mysql.session'@'localhost' failed";
    }
    close_handle();
    return false;
  }
  if (command_query_srv->query(mysql_h, query.data(), query.length())) {
    char err_buf[MYSQL_ERRMSG_SIZE];
    err_buf[0] = '\0';
    char *err_ptr = err_buf;
    const bool got_err =
        !command_error_info_srv->sql_error(mysql_h, &err_ptr);
    if (err_msg != nullptr) {
      if (got_err && err_buf[0] != '\0') {
        *err_msg = std::string("SELECT failed: ") + err_buf;
      } else {
        *err_msg = "SELECT failed";
      }
    }
    close_handle();
    return false;
  }
  if (command_query_result_srv->store_result(mysql_h, &mysql_res)) {
    if (err_msg != nullptr) *err_msg = "store_result failed";
    close_handle();
    return false;
  }
  if (mysql_res == nullptr) {
    close_handle();
    return true;
  }

  unsigned int num_columns = 0;
  if (command_field_info_srv->num_fields(mysql_res, &num_columns)) {
    if (err_msg != nullptr) *err_msg = "num_fields failed";
    free_res();
    close_handle();
    return false;
  }
  if (num_columns < 2) {
    if (err_msg != nullptr) {
      *err_msg =
          "mapping table must expose at least 'user_name' and "
          "'channel_name' columns";
    }
    free_res();
    close_handle();
    return false;
  }

  uint64_t row_count = 0;
  if (command_query_srv->affected_rows(mysql_h, &row_count)) {
    if (err_msg != nullptr) *err_msg = "affected_rows failed";
    free_res();
    close_handle();
    return false;
  }

  for (uint64_t r = 0; r < row_count; ++r) {
    MYSQL_ROW_H row = nullptr;
    if (command_query_result_srv->fetch_row(mysql_res, &row)) {
      if (err_msg != nullptr) *err_msg = "fetch_row failed";
      free_res();
      close_handle();
      return false;
    }
    if (row == nullptr) break;
    const char *u = row[0];
    const char *c = row[1];
    if (u == nullptr || c == nullptr || *u == '\0' || *c == '\0') {
      if (err_msg != nullptr) {
        *err_msg = "mapping table contains NULL or empty user/channel cells";
      }
      free_res();
      close_handle();
      return false;
    }
    out->emplace_back(std::string(u), std::string(c));
  }

  free_res();
  close_handle();
  return true;
}

}  // namespace

bool looks_like_table_uri(const char *spec) {
  return spec != nullptr &&
         std::strncmp(spec, kTableUriPrefix, kTableUriPrefixLen) == 0;
}

bool parse_table_uri(const char *spec, std::string *db, std::string *tbl,
                     std::string *err_msg) {
  if (!looks_like_table_uri(spec)) {
    if (err_msg != nullptr) {
      *err_msg = "expected URI of the form 'table://<db>.<tbl>'";
    }
    return false;
  }
  const std::string rest = spec + kTableUriPrefixLen;
  const auto dot = rest.find('.');
  if (dot == std::string::npos) {
    if (err_msg != nullptr) {
      *err_msg = "table:// URI is missing the '.' between db and table name";
    }
    return false;
  }
  std::string d = rest.substr(0, dot);
  std::string t = rest.substr(dot + 1);
  if (!is_bare_identifier(d) || !is_bare_identifier(t)) {
    if (err_msg != nullptr) {
      *err_msg =
          "table:// db/table identifiers must match [A-Za-z0-9_$]+ and be "
          "<= 64 chars";
    }
    return false;
  }
  if (db != nullptr) *db = std::move(d);
  if (tbl != nullptr) *tbl = std::move(t);
  return true;
}

long long apply_spec_no_sql(const char *spec, std::string *err_msg) {
  if (spec == nullptr || spec[0] == '\0') {
    ArchiveSender::instance().user_channel_map().clear();
    return 0;
  }

  if (looks_like_table_uri(spec)) {
    std::string ignored;
    if (!parse_table_uri(spec, nullptr, nullptr,
                         err_msg != nullptr ? err_msg : &ignored)) {
      return -1;
    }
    return -2;
  }

  std::vector<std::pair<std::string, std::string>> pairs;
  if (!parse_csv(spec, &pairs, err_msg)) return -1;
  if (!ArchiveSender::instance().user_channel_map().set_from_pairs(pairs)) {
    if (err_msg != nullptr) {
      *err_msg = "rejected: empty user/channel cell in mapping";
    }
    return -1;
  }
  return static_cast<long long>(pairs.size());
}

long long apply_spec(const char *spec, std::string *err_msg) {
  if (spec == nullptr || spec[0] == '\0') {
    ArchiveSender::instance().user_channel_map().clear();
    return 0;
  }

  std::vector<std::pair<std::string, std::string>> pairs;

  if (looks_like_table_uri(spec)) {
    std::string db, tbl;
    if (!parse_table_uri(spec, &db, &tbl, err_msg)) return -1;

    MYSQL_THD thd = nullptr;
    if (current_thread_reader_srv->get(&thd) || thd == nullptr) {
      if (err_msg != nullptr) {
        *err_msg =
            "no SQL session context (likely server-startup auto-load); "
            "re-run SET GLOBAL binlog_server.user_channel_map or call "
            "binlog_server_reload_user_channel_map() after startup";
      }
      return -1;
    }

    std::string query;
    query.reserve(64 + db.size() + tbl.size());
    query.append("SELECT user_name, channel_name FROM `");
    query.append(db);
    query.append("`.`");
    query.append(tbl);
    query.append("`");

    if (!run_select(query, &pairs, err_msg)) return -1;
  } else {
    if (!parse_csv(spec, &pairs, err_msg)) return -1;
  }

  if (!ArchiveSender::instance().user_channel_map().set_from_pairs(pairs)) {
    if (err_msg != nullptr) {
      *err_msg = "rejected: empty user/channel cell in mapping";
    }
    return -1;
  }
  return static_cast<long long>(pairs.size());
}

}  // namespace user_channel_map_loader
}  // namespace binlog_server
