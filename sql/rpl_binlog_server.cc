/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

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

#include "sql/rpl_binlog_server.h"

#include <mysql/components/my_service.h>
#include <mysql/components/services/binlog_server_storage.h>
#include <mysql/components/services/log_builtins.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "mysqld_error.h"
#include "sql/mysqld.h"
#include "sql/rpl_mi.h"
#include "sql/rpl_msr.h"

bool binlog_server_storage_uri_shape_ok(const char *uri) {
  if (uri == nullptr || uri[0] == '\0') return false;

  if (std::isalpha(static_cast<unsigned char>(uri[0])) == 0) return false;

  size_t i = 1;
  while (uri[i] != '\0' && uri[i] != ':') {
    const unsigned char c = static_cast<unsigned char>(uri[i]);
    if (std::isalnum(c) == 0 && c != '+' && c != '-' && c != '.') return false;
    ++i;
  }

  if (uri[i] != ':' || uri[i + 1] != '/' || uri[i + 2] != '/' ||
      uri[i + 3] == '\0') {
    return false;
  }
  return true;
}

namespace {

void bslog(enum loglevel level, const char *format, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

void bslog(enum loglevel level, const char *format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  LogErr(level, ER_LOG_PRINTF_MSG, buf);
}

void bslog_code(enum loglevel level, unsigned int errcode, const char *format,
                ...) MY_ATTRIBUTE((format(printf, 3, 4)));

void bslog_code(enum loglevel level, unsigned int errcode, const char *format,
                ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  LogErr(level, errcode, buf);
}

void apply_config(const char *channel, int enabled, const char *uri) {
  if (channel == nullptr) channel = "";

  if (srv_registry == nullptr) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_SERVICE_UNAVAILABLE,
               "cannot configure channel '%s': "
               "component registry unavailable",
               channel);
    return;
  }

  my_service<SERVICE_TYPE(binlog_server_storage)> storage(
      "binlog_server_storage", srv_registry);
  if (!storage.is_valid()) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_SERVICE_UNAVAILABLE,
               "cannot configure channel '%s': "
               "binlog_server_storage service not available "
               "(is component_binlog_server installed?)",
               channel);
    return;
  }

  const int rc = storage->configure_channel(channel, enabled, uri);
  if (rc != 0) {
    bslog(WARNING_LEVEL,
          "binlog_server: configure_channel('%s', enabled=%d, uri='%s') "
          "failed with rc=%d",
          channel, enabled, uri != nullptr ? uri : "(default)", rc);
  } else {
    bslog(INFORMATION_LEVEL,
          "binlog_server: configured channel '%s' (enabled=%d, uri='%s')",
          channel, enabled, uri != nullptr ? uri : "(default)");
  }
}

}  // namespace

void binlog_server_notify_channel_config(Master_info *mi) {
  if (mi == nullptr) return;

  const char *channel = mi->get_channel();
  const char *uri = mi->get_binlog_server_storage_uri();
  if (uri != nullptr && uri[0] == '\0') uri = nullptr;
  const int enabled = mi->is_binlog_server_mode() ? 1 : 0;

  apply_config(channel, enabled, uri);
}

void binlog_server_notify_channel_config_by_name(const char *channel_name) {
  if (channel_name == nullptr || channel_name[0] == '\0') return;

  std::string uri_copy;
  bool has_uri = false;
  bool enabled = false;
  bool found = false;

  channel_map.rdlock();
  if (Master_info *mi = channel_map.get_mi(channel_name)) {
    found = true;
    enabled = mi->is_binlog_server_mode();
    const char *uri = mi->get_binlog_server_storage_uri();
    if (uri != nullptr && uri[0] != '\0') {
      uri_copy = uri;
      has_uri = true;
    }
  }
  channel_map.unlock();

  if (!found) {
    bslog(WARNING_LEVEL,
          "binlog_server: IO thread start on unknown channel '%s'",
          channel_name);
    return;
  }

  if (!enabled) return;

  apply_config(channel_name, 1, has_uri ? uri_copy.c_str() : nullptr);
}

unsigned int binlog_server_reconfigure_all_channels() {
  if (srv_registry == nullptr) {
    bslog(WARNING_LEVEL,
          "binlog_server: reconfigure_all: registry unavailable");
    return 0;
  }

  my_service<SERVICE_TYPE(binlog_server_storage)> storage(
      "binlog_server_storage", srv_registry);
  if (!storage.is_valid()) {
    bslog(WARNING_LEVEL,
          "binlog_server: reconfigure_all: storage service unavailable");
    return 0;
  }

  struct Snapshot {
    std::string name;
    std::string uri;
    bool has_uri{false};
  };
  std::vector<Snapshot> snapshots;

  channel_map.rdlock();
  for (mi_map::iterator it = channel_map.begin(SLAVE_REPLICATION_CHANNEL);
       it != channel_map.end(SLAVE_REPLICATION_CHANNEL); ++it) {
    Master_info *mi = it->second;
    if (mi == nullptr || !mi->is_binlog_server_mode()) continue;

    Snapshot snap;
    const char *cname = mi->get_channel();
    snap.name = cname != nullptr ? cname : "";
    const char *uri = mi->get_binlog_server_storage_uri();
    if (uri != nullptr && uri[0] != '\0') {
      snap.uri = uri;
      snap.has_uri = true;
    }
    snapshots.push_back(std::move(snap));
  }
  channel_map.unlock();

  if (snapshots.empty()) return 0;

  unsigned int configured = 0;
  for (const auto &snap : snapshots) {
    const char *uri_c = snap.has_uri ? snap.uri.c_str() : nullptr;
    if (storage->configure_channel(snap.name.c_str(), 1, uri_c) == 0)
      ++configured;
  }

  bslog(INFORMATION_LEVEL,
        "binlog_server: reconfigure_all: recovered %u/%zu channel(s)",
        configured, snapshots.size());
  return configured;
}
