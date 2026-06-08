/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/binlog_server_storage.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/components/services/mysql_binlog_dump_handler_io.h>
#include <mysql/components/services/mysql_binlog_dump_handler_register.h>
#include <mysql/components/services/mysql_thd_kill_handler.h>
#include <mysqld_error.h>

#include <cstring>
#include <string>

#include "archive_sender.h"
#include "binlog_archive.h"
#include "log_helpers.h"

// Service placeholders required by the component framework
REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_binlog_dump_handler_register,
                                dump_handler_register_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_binlog_dump_handler_io,
                                dump_handler_io_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_thd_kill_handler,
                                thd_kill_handler_srv);

// LogComponentErr uses these global pointers
SERVICE_TYPE(log_builtins) *log_bi;
SERVICE_TYPE(log_builtins_string) *log_bs;

// Global archive instance
static binlog_server::BinlogArchive *g_archive = nullptr;

// System variables
static char *sysvar_default_storage_uri = nullptr;
static char *sysvar_default_serve_channel = nullptr;

static void default_storage_uri_update(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                       const void *save) {
  const char *new_val = *static_cast<const char *const *>(save);
  *static_cast<const char **>(var_ptr) = new_val;
  if (g_archive) {
    g_archive->set_default_storage_uri(new_val != nullptr ? new_val : "");
  }
}

static void default_serve_channel_update(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                         const void *save) {
  const char *new_val = *static_cast<const char *const *>(save);
  *static_cast<const char **>(var_ptr) = new_val;
  binlog_server::ArchiveSender::instance().set_default_channel(
      new_val != nullptr ? new_val : "");
}

static bool archive_sender_dispatch(void *user_data, MYSQL_THD thd,
                                    const char *log_ident, uint64_t pos,
                                    const char *replica_executed_gtids_text,
                                    uint32_t flags, uint32_t source_server_id,
                                    uint32_t replica_server_id,
                                    enum mysql_binlog_dump_handler_checksum_alg
                                        negotiated_checksum_alg,
                                    const char *replica_user) {
  (void)user_data;
  return binlog_server::ArchiveSender::instance().handle(
      thd, log_ident, pos, replica_executed_gtids_text, flags,
      source_server_id, replica_server_id, negotiated_checksum_alg,
      replica_user);
}

// =====================================================
// binlog_server_storage service implementation
// =====================================================

static DEFINE_METHOD(int, bs_configure_channel,
                     (const char *channel_name, int enabled,
                      const char *storage_uri)) {
  if (!g_archive) return 1;
  return g_archive->configure_channel(channel_name, enabled, storage_uri);
}

static DEFINE_METHOD(int, bs_append_event,
                     (const char *channel_name, const char *event_buf,
                      unsigned long event_len)) {
  if (!g_archive) return 1;
  return g_archive->append_event(channel_name, event_buf, event_len);
}

static DEFINE_METHOD(int, bs_close_channel, (const char *channel_name)) {
  if (!g_archive) return 1;
  return g_archive->close_channel(channel_name);
}

static DEFINE_METHOD(int, bs_reset_channel, (const char *channel_name)) {
  if (!g_archive) return 1;
  return g_archive->reset_channel(channel_name);
}

static DEFINE_METHOD(int, bs_flush_channel, (const char *channel_name)) {
  if (!g_archive) return 1;
  return g_archive->flush_channel(channel_name);
}

static DEFINE_METHOD(int, bs_rotate_channel,
                     (const char *channel_name, const char *new_log_name)) {
  if (!g_archive) return 1;
  return g_archive->rotate_channel(channel_name, new_log_name);
}

BEGIN_SERVICE_IMPLEMENTATION(component_binlog_server, binlog_server_storage)
bs_configure_channel, bs_append_event, bs_close_channel, bs_reset_channel,
    bs_flush_channel, bs_rotate_channel END_SERVICE_IMPLEMENTATION();

// =====================================================
// Component lifecycle
// =====================================================

static mysql_service_status_t component_init() {
  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;

  g_archive = new (std::nothrow) binlog_server::BinlogArchive();
  if (!g_archive) return 1;

  // Register system variable: binlog_server.default_storage_uri
  STR_CHECK_ARG(str) str_arg;
  str_arg.def_val = nullptr;
  if (mysql_service_component_sys_variable_register->register_variable(
          "binlog_server", "default_storage_uri",
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
          "Default storage URI for binlog server channels", nullptr,
          default_storage_uri_update, (void *)&str_arg,
          (void *)&sysvar_default_storage_uri)) {
    binlog_server::bslog(ERROR_LEVEL,
                         "binlog_server: failed to register default_storage_uri"
                         " system variable");
  }

  // Register system variable: binlog_server.default_serve_channel
  {
    STR_CHECK_ARG(str) serve_arg;
    serve_arg.def_val = nullptr;
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server", "default_serve_channel",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Default channel name for serving dump connections", nullptr,
            default_serve_channel_update, (void *)&serve_arg,
            (void *)&sysvar_default_serve_channel)) {
      binlog_server::bslog(ERROR_LEVEL,
                           "binlog_server: failed to register "
                           "default_serve_channel system variable");
    }
  }

  // Wire up ArchiveSender
  binlog_server::ArchiveSender::instance().set_storage(g_archive);
  if (dump_handler_register_srv->install(&archive_sender_dispatch, nullptr)) {
    binlog_server::bslog(WARNING_LEVEL,
                         "binlog_server: failed to install dump handler "
                         "(another handler may already be installed)");
  }

  binlog_server::bslog(INFORMATION_LEVEL,
                       "binlog_server: component initialized");
  return 0;
}

static mysql_service_status_t component_deinit() {
  (void)dump_handler_register_srv->uninstall();
  binlog_server::ArchiveSender::instance().set_storage(nullptr);

  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server", "default_serve_channel");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server", "default_storage_uri");

  delete g_archive;
  g_archive = nullptr;

  binlog_server::bslog(INFORMATION_LEVEL,
                       "binlog_server: component deinitialized");
  return 0;
}

// =====================================================
// Component declaration
// =====================================================

BEGIN_COMPONENT_PROVIDES(component_binlog_server)
PROVIDES_SERVICE(component_binlog_server, binlog_server_storage),
    END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(component_binlog_server)
REQUIRES_SERVICE(log_builtins), REQUIRES_SERVICE(log_builtins_string),
    REQUIRES_SERVICE(component_sys_variable_register),
    REQUIRES_SERVICE(component_sys_variable_unregister),
    REQUIRES_SERVICE_AS(mysql_binlog_dump_handler_register,
                        dump_handler_register_srv),
    REQUIRES_SERVICE_AS(mysql_binlog_dump_handler_io, dump_handler_io_srv),
    REQUIRES_SERVICE_AS(mysql_thd_kill_handler, thd_kill_handler_srv),
    END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(component_binlog_server)
METADATA("mysql.author", "Percona LLC"),
    METADATA("mysql.license", "GPL"),
    METADATA("mysql.dev", "Percona"),
    END_COMPONENT_METADATA();

DECLARE_COMPONENT(component_binlog_server, "mysql:binlog_server")
component_init, component_deinit END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(component_binlog_server)
    END_DECLARE_LIBRARY_COMPONENTS
