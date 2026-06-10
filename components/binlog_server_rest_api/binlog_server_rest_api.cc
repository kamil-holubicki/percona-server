/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#define LOG_COMPONENT_TAG "binlog_server_rest_api"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/components/services/mysql_command_services.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysqld_error.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "rest_handlers.h"

// Service placeholders
REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);

REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_factory, rest_command_factory_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_options, rest_command_options_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query, rest_command_query_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_query_result,
                                rest_command_query_result_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_field_info,
                                rest_command_field_info_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_error_info,
                                rest_command_error_info_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_command_thread,
                                rest_command_thread_srv);
REQUIRES_SERVICE_PLACEHOLDER(mysql_command_field_metadata);
REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_current_thread_reader,
                                rest_current_thread_reader_srv);

SERVICE_TYPE(log_builtins) *log_bi;
SERVICE_TYPE(log_builtins_string) *log_bs;

static void rest_log(loglevel level, const char *fmt, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

static void rest_log(loglevel level, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  LogComponentErr(level, ER_LOG_PRINTF_MSG, buf);
}

// System variables
static unsigned int sysvar_port = 8440;
static char *sysvar_bind_address = nullptr;
static char *sysvar_username = nullptr;
static char *sysvar_password = nullptr;
static char *sysvar_ssl_cert = nullptr;
static char *sysvar_ssl_key = nullptr;

static const char *default_bind_address = "127.0.0.1";
static const char *default_username = "admin";
static const char *default_password = "admin";
static const char *default_ssl_cert = "";
static const char *default_ssl_key = "";

// Server state
static std::unique_ptr<httplib::Server> g_server;
static std::unique_ptr<httplib::SSLServer> g_ssl_server;
static std::thread g_server_thread;
static std::atomic<bool> g_running{false};
static std::string g_plugin_dir;

static std::string detect_plugin_dir() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(detect_plugin_dir), &info) &&
      info.dli_fname != nullptr) {
    std::string so_path(info.dli_fname);
    auto slash = so_path.rfind('/');
    if (slash != std::string::npos) return so_path.substr(0, slash + 1);
  }
  return "./";
}


static httplib::Server *get_active_server() {
  if (g_ssl_server) return g_ssl_server.get();
  return g_server.get();
}

static void server_thread_func() {
  auto *svr = get_active_server();
  if (!svr) return;

  std::string bind = sysvar_bind_address ? sysvar_bind_address : "127.0.0.1";
  int port = static_cast<int>(sysvar_port);

  rest_log(INFORMATION_LEVEL, "REST API listening on %s:%d%s", bind.c_str(),
           port, g_ssl_server ? " (HTTPS)" : "");

  if (!svr->listen(bind, port)) {
    if (g_running.load()) {
      rest_log(ERROR_LEVEL, "REST API server listen() failed on %s:%d",
               bind.c_str(), port);
    }
  }
}

static bool start_server() {
  std::string cert = sysvar_ssl_cert ? sysvar_ssl_cert : "";
  std::string key = sysvar_ssl_key ? sysvar_ssl_key : "";
  std::string user = sysvar_username ? sysvar_username : "admin";
  std::string pass = sysvar_password ? sysvar_password : "";

  if (pass.empty()) {
    rest_log(ERROR_LEVEL,
             "REST API cannot start: binlog_server_rest_api.password is empty");
    return false;
  }

  httplib::Server *svr = nullptr;

  if (!cert.empty() && !key.empty()) {
    g_ssl_server =
        std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str());
    if (!g_ssl_server->is_valid()) {
      rest_log(ERROR_LEVEL,
               "REST API HTTPS setup failed (check cert/key paths)");
      g_ssl_server.reset();
      return false;
    }
    svr = g_ssl_server.get();
  } else {
    g_server = std::make_unique<httplib::Server>();
    svr = g_server.get();
  }

  // Basic Auth middleware (skip /api/v1/health and dashboard)
  std::string expected_auth =
      "Basic " + httplib::detail::base64_encode(user + ":" + pass);
  svr->set_pre_routing_handler(
      [expected_auth](const httplib::Request &req, httplib::Response &res) {
        if (req.path == "/api/v1/health" || req.path == "/") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        auto auth = req.get_header_value("Authorization");
        if (auth.empty()) {
          res.status = 401;
          res.set_header("WWW-Authenticate", "Basic realm=\"Binlog Server\"");
          res.set_content("{\"error\":\"authentication required\"}",
                          "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }
        if (auth != expected_auth) {
          res.status = 401;
          res.set_header("WWW-Authenticate", "Basic realm=\"Binlog Server\"");
          res.set_content("{\"error\":\"invalid credentials\"}",
                          "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  // CORS headers for browser dashboard
  svr->set_post_routing_handler(
      [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods",
                       "GET, POST, PUT, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       "Authorization, Content-Type");
      });

  // Register all endpoint handlers
  rest_api::register_handlers(*svr, g_plugin_dir);

  g_running.store(true);
  g_server_thread = std::thread(server_thread_func);

  return true;
}

static void stop_server() {
  g_running.store(false);
  auto *svr = get_active_server();
  if (svr) svr->stop();
  if (g_server_thread.joinable()) g_server_thread.join();
  g_server.reset();
  g_ssl_server.reset();
}

static void password_update(MYSQL_THD, SYS_VAR *, void *val_ptr,
                            const void *save) {
  const char *new_val = *static_cast<const char *const *>(save);
  *static_cast<const char **>(val_ptr) = new_val;

  if (!g_running.load() && new_val != nullptr && new_val[0] != '\0') {
    start_server();
  }
}

// Component init
static mysql_service_status_t component_init() {
  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;

  g_plugin_dir = detect_plugin_dir();

  // Register system variables
  {
    STR_CHECK_ARG(str) arg;
    arg.def_val = const_cast<char *>(default_bind_address);
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "bind_address",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Bind address for REST API server", nullptr, nullptr,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_bind_address))) {
      rest_log(ERROR_LEVEL, "failed to register bind_address sysvar");
      return 1;
    }
  }
  {
    INTEGRAL_CHECK_ARG(uint) arg;
    arg.def_val = 8440;
    arg.min_val = 1024;
    arg.max_val = 65535;
    arg.blk_sz = 0;
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "port",
            PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_RQCMDARG,
            "TCP port for REST API server", nullptr, nullptr,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_port))) {
      rest_log(ERROR_LEVEL, "failed to register port sysvar");
      return 1;
    }
  }
  {
    STR_CHECK_ARG(str) arg;
    arg.def_val = const_cast<char *>(default_username);
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "username",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Basic Auth username for REST API", nullptr, nullptr,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_username))) {
      rest_log(ERROR_LEVEL, "failed to register username sysvar");
      return 1;
    }
  }
  {
    STR_CHECK_ARG(str) arg;
    arg.def_val = const_cast<char *>(default_password);
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "password",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Basic Auth password for REST API", nullptr, password_update,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_password))) {
      rest_log(ERROR_LEVEL, "failed to register password sysvar");
      return 1;
    }
  }
  {
    STR_CHECK_ARG(str) arg;
    arg.def_val = const_cast<char *>(default_ssl_cert);
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "ssl_cert",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Path to PEM certificate (enables HTTPS)", nullptr, nullptr,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_ssl_cert))) {
      rest_log(ERROR_LEVEL, "failed to register ssl_cert sysvar");
      return 1;
    }
  }
  {
    STR_CHECK_ARG(str) arg;
    arg.def_val = const_cast<char *>(default_ssl_key);
    if (mysql_service_component_sys_variable_register->register_variable(
            "binlog_server_rest_api", "ssl_key",
            PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_RQCMDARG,
            "Path to PEM private key (enables HTTPS)", nullptr, nullptr,
            reinterpret_cast<void *>(&arg),
            reinterpret_cast<void *>(&sysvar_ssl_key))) {
      rest_log(ERROR_LEVEL, "failed to register ssl_key sysvar");
      return 1;
    }
  }

  if (!start_server()) {
    rest_log(WARNING_LEVEL,
             "REST API server not started (set password and restart)");
  }

  rest_log(INFORMATION_LEVEL, "binlog_server_rest_api component loaded");
  return 0;
}

// Component deinit
static mysql_service_status_t component_deinit() {
  stop_server();

  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "bind_address");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "port");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "username");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "password");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "ssl_cert");
  mysql_service_component_sys_variable_unregister->unregister_variable(
      "binlog_server_rest_api", "ssl_key");

  rest_log(INFORMATION_LEVEL, "binlog_server_rest_api component unloaded");
  return 0;
}

// Component declaration
BEGIN_COMPONENT_PROVIDES(component_binlog_server_rest_api)
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(component_binlog_server_rest_api)
REQUIRES_SERVICE(log_builtins),
    REQUIRES_SERVICE(log_builtins_string),
    REQUIRES_SERVICE(component_sys_variable_register),
    REQUIRES_SERVICE(component_sys_variable_unregister),
    REQUIRES_SERVICE_AS(mysql_command_factory, rest_command_factory_srv),
    REQUIRES_SERVICE_AS(mysql_command_options, rest_command_options_srv),
    REQUIRES_SERVICE_AS(mysql_command_query, rest_command_query_srv),
    REQUIRES_SERVICE_AS(mysql_command_query_result,
                        rest_command_query_result_srv),
    REQUIRES_SERVICE_AS(mysql_command_field_info, rest_command_field_info_srv),
    REQUIRES_SERVICE_AS(mysql_command_error_info, rest_command_error_info_srv),
    REQUIRES_SERVICE_AS(mysql_command_thread, rest_command_thread_srv),
    REQUIRES_SERVICE(mysql_command_field_metadata),
    REQUIRES_SERVICE_AS(mysql_current_thread_reader,
                        rest_current_thread_reader_srv),
    END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(component_binlog_server_rest_api)
METADATA("mysql.author", "Percona LLC"),
    METADATA("mysql.license", "GPL"),
    METADATA("mysql.version", "1.0"),
    END_COMPONENT_METADATA();

DECLARE_COMPONENT(component_binlog_server_rest_api,
                  "mysql:binlog_server_rest_api")
component_init, component_deinit END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(component_binlog_server_rest_api)
    END_DECLARE_LIBRARY_COMPONENTS
