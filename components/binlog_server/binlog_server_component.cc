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
#include <mysql/components/services/mysql_command_services.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_thd_kill_handler.h>
#include <mysql/components/services/udf_registration.h>
#include <mysqld_error.h>

#include <mysql/components/services/bits/my_err_bits.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "archive_sender.h"
#include "binlog_archive.h"
#include "log_helpers.h"
#include "user_channel_map_loader.h"

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

// table:// support for binlog_server.user_channel_map
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_factory, command_factory_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_options, command_options_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query, command_query_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query_result,
                                command_query_result_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_field_info,
                                command_field_info_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_error_info,
                                command_error_info_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_current_thread_reader,
                                current_thread_reader_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(udf_registration, udf_registration_srv);

// LogComponentErr uses these global pointers
SERVICE_TYPE(log_builtins) *log_bi;
SERVICE_TYPE(log_builtins_string) *log_bs;

// Global archive instance
static binlog_server::BinlogArchive *g_archive = nullptr;

// System variables
static char *sysvar_default_storage_uri = nullptr;
static char *sysvar_default_serve_channel = nullptr;
static char *sysvar_user_channel_map = nullptr;

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

static int user_channel_map_check(MYSQL_THD, SYS_VAR *, void *save,
                                  st_mysql_value *value) {
  int len = 0;
  const char *raw = value->val_str(value, nullptr, &len);
  const char *spec = (raw == nullptr) ? "" : raw;

  if (binlog_server::user_channel_map_loader::looks_like_table_uri(spec)) {
    std::string err;
    if (!binlog_server::user_channel_map_loader::parse_table_uri(
            spec, nullptr, nullptr, &err)) {
      binlog_server::bslog(WARNING_LEVEL,
                           "binlog_server: user_channel_map rejected: %s",
                           err.c_str());
      return 1;
    }
  } else if (spec[0] != '\0') {
    binlog_server::UserChannelMap probe;
    if (!probe.set_from_csv(spec)) {
      binlog_server::bslog(
          WARNING_LEVEL,
          "binlog_server: user_channel_map rejected (expected "
          "'user1=channel1,user2=channel2,...' or 'table://<db>.<tbl>')");
      return 1;
    }
  }
  *static_cast<const char **>(save) = raw;
  return 0;
}

static void user_channel_map_update(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                    const void *save) {
  const char *new_val = *static_cast<const char *const *>(save);
  *static_cast<const char **>(var_ptr) = new_val;

  std::string err;
  const long long n =
      binlog_server::user_channel_map_loader::apply_spec_no_sql(
          sysvar_user_channel_map, &err);
  if (n == -2) {
    binlog_server::bslog(
        INFORMATION_LEVEL,
        "binlog_server: user_channel_map='%s' stored; call "
        "binlog_server_reload_user_channel_map() to load the rows "
        "(or restart the component / re-INSTALL to load at init time)",
        sysvar_user_channel_map != nullptr ? sysvar_user_channel_map : "");
  } else if (n < 0) {
    binlog_server::bslog(
        WARNING_LEVEL,
        "binlog_server: user_channel_map update accepted but applying "
        "the new spec failed (%s); keeping the previously-loaded map. "
        "Call binlog_server_reload_user_channel_map() to retry.",
        err.c_str());
  } else {
    binlog_server::bslog(INFORMATION_LEVEL,
                         "binlog_server: user_channel_map applied "
                         "(%lld mappings)",
                         n);
  }
}

// =====================================================
// UDF: binlog_server_reload_user_channel_map()
// =====================================================

extern "C" {

static bool binlog_server_reload_ucm_init(UDF_INIT *initid, UDF_ARGS *args,
                                          char *message) {
  if (args->arg_count != 0) {
    std::snprintf(
        message, MYSQL_ERRMSG_SIZE,
        "binlog_server_reload_user_channel_map() takes no arguments");
    return true;
  }
  initid->maybe_null = true;
  return false;
}

static long long binlog_server_reload_ucm_func(UDF_INIT *, UDF_ARGS *,
                                               unsigned char *is_null,
                                               unsigned char *error) {
  std::string err;
  const long long n =
      binlog_server::user_channel_map_loader::apply_spec(
          sysvar_user_channel_map, &err);
  if (n < 0) {
    binlog_server::bslog(
        WARNING_LEVEL,
        "binlog_server: binlog_server_reload_user_channel_map() failed: "
        "%s; keeping the previously-loaded map.",
        err.c_str());
    *is_null = 1;
    *error = 0;
    return 0;
  }
  *is_null = 0;
  *error = 0;
  binlog_server::bslog(
      INFORMATION_LEVEL,
      "binlog_server: binlog_server_reload_user_channel_map() applied "
      "(%lld mappings)",
      n);
  return n;
}

static void binlog_server_reload_ucm_deinit(UDF_INIT *) {}

}  // extern "C"

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
          "Default storage URI for binlog server channels. "
          "Currently only 'file://<absolute-path>/' is supported. "
          "Empty clears the default.",
          nullptr, default_storage_uri_update, (void *)&str_arg,
          (void *)&sysvar_default_storage_uri)) {
    binlog_server::bslog(ERROR_LEVEL,
                         "binlog_server: failed to register default_storage_uri"
                         " system variable");
    delete g_archive;
    g_archive = nullptr;
    return 1;
  }

  // Register system variable: binlog_server.default_serve_channel
  {
    STR_CHECK_ARG(str) serve_arg;
    serve_arg.def_val = nullptr;
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server", "default_serve_channel",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Name of the collected channel to serve to replicas that do "
            "not match any user_channel_map entry. Empty disables the "
            "fallback (unmapped users fall through to default Binlog_sender).",
            nullptr, default_serve_channel_update, (void *)&serve_arg,
            (void *)&sysvar_default_serve_channel)) {
      binlog_server::bslog(ERROR_LEVEL,
                           "binlog_server: failed to register "
                           "default_serve_channel system variable");
      delete g_archive;
      g_archive = nullptr;
      return 1;
    }
  }

  // Register system variable: binlog_server.user_channel_map
  {
    STR_CHECK_ARG(str) ucm_arg;
    ucm_arg.def_val = nullptr;
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server", "user_channel_map",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "User -> collected-channel mapping consulted when a replica "
            "connects. Two shapes: (1) inline CSV "
            "'user1=channel1,user2=channel2,...' -- applied immediately "
            "at SET GLOBAL time; or (2) 'table://<db>.<tbl>' pointing at "
            "a local table with columns 'user_name' VARCHAR and "
            "'channel_name' VARCHAR. For the table shape the read is "
            "performed as 'mysql.session'@'localhost' (requires GRANT "
            "SELECT); the table is fetched at component_init and when "
            "the operator calls "
            "binlog_server_reload_user_channel_map(). SET GLOBAL of a "
            "table:// URI stores the spec but does NOT contact the SQL "
            "engine (deferred due to LOCK_plugin).",
            user_channel_map_check, user_channel_map_update,
            (void *)&ucm_arg, (void *)&sysvar_user_channel_map)) {
      binlog_server::bslog(ERROR_LEVEL,
                           "binlog_server: failed to register "
                           "user_channel_map system variable");
      delete g_archive;
      g_archive = nullptr;
      return 1;
    }
  }

  // Wire up ArchiveSender with storage and persisted default_serve_channel
  binlog_server::ArchiveSender::instance().set_storage(g_archive);
  binlog_server::ArchiveSender::instance().set_default_channel(
      sysvar_default_serve_channel != nullptr ? sysvar_default_serve_channel
                                              : "");

  // Install dump handler -- hard failure if another handler is installed
  if (dump_handler_register_srv->install(&archive_sender_dispatch, nullptr)) {
    binlog_server::bslog(ERROR_LEVEL,
                         "binlog_server: another binlog dump handler is "
                         "already installed; refusing to load");
    delete g_archive;
    g_archive = nullptr;
    return 1;
  }

  // Register reload UDF
  if (udf_registration_srv->udf_register(
          "binlog_server_reload_user_channel_map", INT_RESULT,
          reinterpret_cast<Udf_func_any>(binlog_server_reload_ucm_func),
          binlog_server_reload_ucm_init,
          binlog_server_reload_ucm_deinit)) {
    binlog_server::bslog(WARNING_LEVEL,
                         "binlog_server: failed to register "
                         "binlog_server_reload_user_channel_map() UDF");
  }

  // Apply initial user_channel_map spec (CSV applies; table:// may fail
  // at startup auto-load when there is no SQL context)
  {
    std::string err;
    const long long n =
        binlog_server::user_channel_map_loader::apply_spec(
            sysvar_user_channel_map, &err);
    if (n < 0) {
      binlog_server::bslog(
          WARNING_LEVEL,
          "binlog_server: initial user_channel_map apply failed (%s); "
          "live map is empty. Re-run SET GLOBAL "
          "binlog_server.user_channel_map or call "
          "binlog_server_reload_user_channel_map() to retry.",
          err.c_str());
    } else if (n > 0) {
      binlog_server::bslog(INFORMATION_LEVEL,
                           "binlog_server: initial user_channel_map applied "
                           "(%lld mappings)",
                           n);
    }
  }

  binlog_server::bslog(INFORMATION_LEVEL,
                       "binlog_server: component initialized");
  return 0;
}

static mysql_service_status_t component_deinit() {
  (void)dump_handler_register_srv->uninstall();
  binlog_server::ArchiveSender::instance().set_storage(nullptr);

  // Unregister UDF first -- if a session is mid-call, refuse unload
  {
    int was_present = 0;
    bool unregistered = false;
    for (int i = 0; i < 10; ++i) {
      if (!udf_registration_srv->udf_unregister(
              "binlog_server_reload_user_channel_map", &was_present)) {
        unregistered = true;
        break;
      }
      if (was_present == 0) {
        unregistered = true;
        break;
      }
    }
    if (!unregistered) {
      binlog_server::bslog(
          WARNING_LEVEL,
          "binlog_server: cannot unregister "
          "binlog_server_reload_user_channel_map() UDF; "
          "it is in use by another session -- refusing unload");
      return 1;
    }
  }

  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server", "user_channel_map");
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
    REQUIRES_SERVICE_AS(mysql_command_factory, command_factory_srv),
    REQUIRES_SERVICE_AS(mysql_command_options, command_options_srv),
    REQUIRES_SERVICE_AS(mysql_command_query, command_query_srv),
    REQUIRES_SERVICE_AS(mysql_command_query_result, command_query_result_srv),
    REQUIRES_SERVICE_AS(mysql_command_field_info, command_field_info_srv),
    REQUIRES_SERVICE_AS(mysql_command_error_info, command_error_info_srv),
    REQUIRES_SERVICE_AS(mysql_current_thread_reader,
                        current_thread_reader_srv),
    REQUIRES_SERVICE_AS(udf_registration, udf_registration_srv),
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
