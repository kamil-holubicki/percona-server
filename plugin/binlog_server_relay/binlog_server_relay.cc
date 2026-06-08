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

#include <mysql/components/my_service.h>
#include <mysql/components/services/binlog_server_storage.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/plugin.h>

#include "mysqld_error.h"
#include "sql/replication.h"
#include "sql/rpl_binlog_server.h"

SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

static SERVICE_TYPE(registry) *reg = nullptr;
static SERVICE_TYPE(registry) *log_reg = nullptr;

// RAII-managed storage service handle.
static my_service<SERVICE_TYPE(binlog_server_storage)> *g_storage = nullptr;

static int bs_relay_thread_start(Binlog_relay_IO_param *param) {
  if (param == nullptr || param->channel_name == nullptr) return 0;
  binlog_server_notify_channel_config_by_name(param->channel_name);
  LogPluginErrMsg(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                  "IO thread started for channel '%s'",
                  param->channel_name);
  return 0;
}

static int bs_relay_thread_stop(Binlog_relay_IO_param *param) {
  if (g_storage == nullptr || !g_storage->is_valid() || param == nullptr ||
      param->channel_name == nullptr)
    return 0;
  int rc = (*g_storage)->close_channel(param->channel_name);
  if (rc != 0) {
    LogPluginErrMsg(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
                    "close_channel failed for '%s' (rc=%d)",
                    param->channel_name, rc);
  }
  LogPluginErrMsg(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                  "IO thread stopped for channel '%s'",
                  param->channel_name);
  return 0;
}

static int bs_relay_after_queue_event(Binlog_relay_IO_param *param,
                                      const char *event_buf,
                                      unsigned long event_len,
                                      uint32 /*flags*/) {
  if (g_storage == nullptr || !g_storage->is_valid() || param == nullptr ||
      param->channel_name == nullptr)
    return 0;
  if (event_buf == nullptr || event_len == 0) return 0;
  int rc = (*g_storage)->append_event(param->channel_name, event_buf,
                                      event_len);
  if (rc != 0) {
    LogPluginErrMsg(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "append_event failed for channel '%s' (rc=%d, len=%lu)",
                    param->channel_name, rc, event_len);
  }
  return rc;
}

static int bs_relay_after_reset_slave(Binlog_relay_IO_param *param) {
  if (g_storage == nullptr || !g_storage->is_valid() || param == nullptr ||
      param->channel_name == nullptr)
    return 0;
  int rc = (*g_storage)->reset_channel(param->channel_name);
  if (rc != 0) {
    LogPluginErrMsg(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
                    "reset_channel failed for '%s' (rc=%d)",
                    param->channel_name, rc);
  }
  return 0;
}

static Binlog_relay_IO_observer relay_observer = {
    sizeof(Binlog_relay_IO_observer),
    bs_relay_thread_start,
    bs_relay_thread_stop,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    bs_relay_after_queue_event,
    bs_relay_after_reset_slave,
    nullptr,
};

static int binlog_server_relay_init(MYSQL_PLUGIN plugin_info) {
  if (init_logging_service_for_plugin(&log_reg, &log_bi, &log_bs)) return 1;

  reg = mysql_plugin_registry_acquire();
  if (reg == nullptr) return 1;

  g_storage = new (std::nothrow)
      my_service<SERVICE_TYPE(binlog_server_storage)>(
          "binlog_server_storage", reg);
  if (g_storage == nullptr || !g_storage->is_valid()) {
    delete g_storage;
    g_storage = nullptr;
    mysql_plugin_registry_release(reg);
    reg = nullptr;
    LogPluginErrMsg(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "cannot acquire binlog_server_storage service. "
                    "Is component_binlog_server installed?");
    return 1;
  }

  if (register_binlog_relay_io_observer(&relay_observer, plugin_info)) {
    delete g_storage;
    g_storage = nullptr;
    mysql_plugin_registry_release(reg);
    reg = nullptr;
    LogPluginErrMsg(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "failed to register IO observer");
    return 1;
  }

  unsigned int recovered = binlog_server_reconfigure_all_channels();
  LogPluginErrMsg(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                  "plugin initialized (reconfigured %u channels)", recovered);
  return 0;
}

static int binlog_server_relay_deinit(MYSQL_PLUGIN plugin_info) {
  if (unregister_binlog_relay_io_observer(&relay_observer, plugin_info)) {
    LogPluginErrMsg(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
                    "failed to unregister IO observer");
  }

  delete g_storage;
  g_storage = nullptr;

  if (reg != nullptr) {
    mysql_plugin_registry_release(reg);
    reg = nullptr;
  }

  LogPluginErrMsg(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                  "plugin deinitialized");

  deinit_logging_service_for_plugin(&log_reg, &log_bi, &log_bs);
  return 0;
}

static struct Mysql_replication binlog_server_relay_descriptor = {
    MYSQL_REPLICATION_INTERFACE_VERSION};

mysql_declare_plugin(binlog_server_relay){
    MYSQL_REPLICATION_PLUGIN,
    &binlog_server_relay_descriptor,
    "binlog_server_relay",
    PLUGIN_AUTHOR_ORACLE,
    "Binlog server IO-thread relay observer",
    PLUGIN_LICENSE_GPL,
    binlog_server_relay_init,
    nullptr,
    binlog_server_relay_deinit,
    0x0100,
    nullptr,
    nullptr,
    nullptr,
    0,
} mysql_declare_plugin_end;
