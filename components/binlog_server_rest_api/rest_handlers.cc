/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "rest_handlers.h"

#include "json_helpers.h"
#include "sql_executor.h"

#include <fstream>
#include <sstream>
#include <string>

namespace rest_api {

namespace {

std::string rows_to_json_array(const SqlResult &res) {
  json::Array arr;
  for (const auto &row : res.rows) {
    json::Object obj;
    for (size_t i = 0; i < row.columns.size() && i < row.values.size(); ++i) {
      obj.add(row.columns[i].c_str(), row.values[i]);
    }
    arr.add(obj.str());
  }
  return arr.str();
}

std::string error_json(const std::string &msg) {
  json::Object o;
  o.add("error", msg);
  return o.str();
}

std::string parse_json_string(const std::string &body, const char *key) {
  std::string search = std::string("\"") + key + "\"";
  auto pos = body.find(search);
  if (pos == std::string::npos) return "";
  pos = body.find(':', pos + search.size());
  if (pos == std::string::npos) return "";
  pos = body.find('"', pos + 1);
  if (pos == std::string::npos) return "";
  ++pos;
  auto end = body.find('"', pos);
  if (end == std::string::npos) return "";
  return body.substr(pos, end - pos);
}

long long parse_json_int(const std::string &body, const char *key) {
  std::string search = std::string("\"") + key + "\"";
  auto pos = body.find(search);
  if (pos == std::string::npos) return 0;
  pos = body.find(':', pos + search.size());
  if (pos == std::string::npos) return 0;
  ++pos;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
  return std::strtoll(body.c_str() + pos, nullptr, 10);
}

std::string sql_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\'')
      out += "''";
    else if (c == '\\')
      out += "\\\\";
    else
      out += c;
  }
  return out;
}

}  // namespace

void register_handlers(httplib::Server &svr,
                       const std::string &plugin_dir) {
  // Health check (no auth)
  svr.Get("/api/v1/health", [](const httplib::Request &,
                               httplib::Response &res) {
    res.set_content("{\"status\":\"ok\"}", "application/json");
  });

  // Dashboard — re-read from disk on every request for live editing
  svr.Get("/", [plugin_dir](const httplib::Request &,
                            httplib::Response &res) {
    std::string path = plugin_dir + "binlog_server_rest_api_dashboard.html";
    std::ifstream f(path);
    if (f.is_open()) {
      std::ostringstream ss;
      ss << f.rdbuf();
      res.set_content(ss.str(), "text/html");
    } else {
      res.set_content(
          "<!DOCTYPE html><html><body><h1>Binlog Server REST API</h1>"
          "<p>Dashboard HTML not found at <code>" + path +
              "</code></p></body></html>",
          "text/html");
    }
  });

  // GET /api/v1/status
  svr.Get("/api/v1/status", [](const httplib::Request &,
                               httplib::Response &res) {
    auto r = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_status");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    res.set_content(rows_to_json_array(r), "application/json");
  });

  // GET /api/v1/status/:channel
  svr.Get("/api/v1/status/:channel", [](const httplib::Request &req,
                                        httplib::Response &res) {
    std::string ch = sql_escape(req.path_params.at("channel"));
    auto r = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_status "
        "WHERE CHANNEL_NAME = '" +
        ch + "'");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    if (r.rows.empty()) {
      res.status = 404;
      res.set_content(error_json("channel not found"), "application/json");
      return;
    }
    json::Object obj;
    for (size_t i = 0; i < r.rows[0].columns.size(); ++i)
      obj.add(r.rows[0].columns[i].c_str(), r.rows[0].values[i]);
    res.set_content(obj.str(), "application/json");
  });

  // GET /api/v1/storage
  svr.Get("/api/v1/storage", [](const httplib::Request &,
                                httplib::Response &res) {
    auto r = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_storage");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    res.set_content(rows_to_json_array(r), "application/json");
  });

  // GET /api/v1/storage/:channel
  svr.Get("/api/v1/storage/:channel", [](const httplib::Request &req,
                                         httplib::Response &res) {
    std::string ch = sql_escape(req.path_params.at("channel"));
    auto r = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_storage "
        "WHERE CHANNEL_NAME = '" +
        ch + "'");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    if (r.rows.empty()) {
      res.status = 404;
      res.set_content(error_json("channel not found"), "application/json");
      return;
    }
    json::Object obj;
    for (size_t i = 0; i < r.rows[0].columns.size(); ++i)
      obj.add(r.rows[0].columns[i].c_str(), r.rows[0].values[i]);
    res.set_content(obj.str(), "application/json");
  });

  // GET /api/v1/archive/:channel
  svr.Get("/api/v1/archive/:channel", [](const httplib::Request &req,
                                         httplib::Response &res) {
    std::string ch = sql_escape(req.path_params.at("channel"));
    auto r = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_archive "
        "WHERE CHANNEL_NAME = '" +
        ch + "' ORDER BY FILE_NAME");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    res.set_content(rows_to_json_array(r), "application/json");
  });

  // GET /api/v1/variables
  svr.Get("/api/v1/variables", [](const httplib::Request &,
                                  httplib::Response &res) {
    auto r = execute_query(
        "SHOW GLOBAL VARIABLES LIKE 'binlog_server%'");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    for (const auto &row : r.rows) {
      if (row.values.size() >= 2)
        obj.add(row.values[0].c_str(), row.values[1]);
    }
    res.set_content(obj.str(), "application/json");
  });

  // GET /api/v1/topology
  svr.Get("/api/v1/topology", [](const httplib::Request &,
                                 httplib::Response &res) {
    // Channels from PFS status
    auto status = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_status");
    auto storage = execute_query(
        "SELECT * FROM performance_schema.replication_binlog_server_storage");

    // Upstream sources: join connection_configuration (has host/port) with
    // connection_status (has state)
    auto replicas = execute_query(
        "SELECT c.CHANNEL_NAME, c.HOST AS SOURCE_HOST, c.PORT AS SOURCE_PORT, "
        "s.SERVICE_STATE "
        "FROM performance_schema.replication_connection_configuration c "
        "JOIN performance_schema.replication_connection_status s "
        "  ON c.CHANNEL_NAME = s.CHANNEL_NAME "
        "WHERE c.CHANNEL_NAME != ''");

    json::Object topo;

    // Build sources array from replication_connection_status
    json::Array sources_arr;
    if (replicas.ok) {
      for (const auto &row : replicas.rows) {
        json::Object src;
        for (size_t i = 0; i < row.columns.size(); ++i)
          src.add(row.columns[i].c_str(), row.values[i]);
        sources_arr.add(src.str());
      }
    }
    topo.add_raw("sources", sources_arr.str());

    // Build channels array from PFS status + storage
    json::Array channels_arr;
    if (status.ok) {
      for (const auto &row : status.rows) {
        json::Object ch;
        for (size_t i = 0; i < row.columns.size(); ++i)
          ch.add(row.columns[i].c_str(), row.values[i]);
        // Merge storage info if available
        if (storage.ok) {
          for (const auto &srow : storage.rows) {
            auto ch_idx =
                std::find(srow.columns.begin(), srow.columns.end(),
                          "CHANNEL_NAME");
            if (ch_idx != srow.columns.end()) {
              size_t ci = ch_idx - srow.columns.begin();
              auto row_ch_idx =
                  std::find(row.columns.begin(), row.columns.end(),
                            "CHANNEL_NAME");
              size_t ri = row_ch_idx - row.columns.begin();
              if (ci < srow.values.size() && ri < row.values.size() &&
                  srow.values[ci] == row.values[ri]) {
                for (size_t i = 0; i < srow.columns.size(); ++i) {
                  if (srow.columns[i] != "CHANNEL_NAME")
                    ch.add(srow.columns[i].c_str(), srow.values[i]);
                }
                break;
              }
            }
          }
        }
        channels_arr.add(ch.str());
      }
    }
    topo.add_raw("channels", channels_arr.str());

    // Downstream replicas: look for Binlog Dump threads.
    // Try performance_schema.threads first, then information_schema.PROCESSLIST
    json::Array down_arr;
    auto downstream = execute_query(
        "SELECT PROCESSLIST_ID AS ID, PROCESSLIST_USER AS USER, "
        "PROCESSLIST_HOST AS HOST, PROCESSLIST_COMMAND AS COMMAND, "
        "PROCESSLIST_STATE AS STATE "
        "FROM performance_schema.threads "
        "WHERE TYPE = 'FOREGROUND' AND PROCESSLIST_COMMAND IN "
        "('Binlog Dump', 'Binlog Dump GTID')");
    if (!downstream.ok || downstream.rows.empty()) {
      downstream = execute_query(
          "SELECT ID, USER, HOST, COMMAND, STATE "
          "FROM information_schema.PROCESSLIST "
          "WHERE COMMAND IN ('Binlog Dump', 'Binlog Dump GTID')");
    }
    if (downstream.ok) {
      for (const auto &row : downstream.rows) {
        json::Object d;
        for (size_t i = 0; i < row.columns.size(); ++i)
          d.add(row.columns[i].c_str(), row.values[i]);
        down_arr.add(d.str());
      }
    }
    topo.add_raw("downstream", down_arr.str());

    res.set_content(topo.str(), "application/json");
  });

  // GET /api/v1/threads — debug: show all foreground threads
  svr.Get("/api/v1/threads", [](const httplib::Request &,
                                httplib::Response &res) {
    auto r = execute_query(
        "SELECT PROCESSLIST_ID, PROCESSLIST_USER, PROCESSLIST_HOST, "
        "PROCESSLIST_COMMAND, PROCESSLIST_STATE, PROCESSLIST_INFO "
        "FROM performance_schema.threads "
        "WHERE TYPE = 'FOREGROUND'");
    if (!r.ok) {
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    res.set_content(rows_to_json_array(r), "application/json");
  });

  // POST /api/v1/purge/by-file
  svr.Post("/api/v1/purge/by-file", [](const httplib::Request &req,
                                       httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    std::string file = sql_escape(parse_json_string(req.body, "file"));
    if (ch.empty() || file.empty()) {
      res.status = 400;
      res.set_content(error_json("channel and file required"),
                      "application/json");
      return;
    }
    auto r = execute_query("SELECT binlog_server_purge_channel('" + ch +
                           "', '" + file + "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("purged", r.rows.empty() ? "0" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/purge/by-gtid
  svr.Post("/api/v1/purge/by-gtid", [](const httplib::Request &req,
                                       httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    std::string gtid = sql_escape(parse_json_string(req.body, "gtid_set"));
    if (ch.empty() || gtid.empty()) {
      res.status = 400;
      res.set_content(error_json("channel and gtid_set required"),
                      "application/json");
      return;
    }
    auto r = execute_query("SELECT binlog_server_purge_before_gtid('" + ch +
                           "', '" + gtid + "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("purged", r.rows.empty() ? "0" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/purge/by-timestamp
  svr.Post("/api/v1/purge/by-timestamp", [](const httplib::Request &req,
                                            httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    long long ts = parse_json_int(req.body, "timestamp");
    if (ch.empty() || ts <= 0) {
      res.status = 400;
      res.set_content(error_json("channel and timestamp required"),
                      "application/json");
      return;
    }
    auto r = execute_query("SELECT binlog_server_purge_before_timestamp('" +
                           ch + "', " + std::to_string(ts) + ") AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("purged", r.rows.empty() ? "0" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/rotate-key
  svr.Post("/api/v1/rotate-key", [](const httplib::Request &req,
                                    httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    if (ch.empty()) {
      res.status = 400;
      res.set_content(error_json("channel required"), "application/json");
      return;
    }
    auto r = execute_query(
        "SELECT binlog_server_rotate_encryption_key('" + ch + "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("new_key", r.rows.empty() ? "" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/reload-map
  svr.Post("/api/v1/reload-map", [](const httplib::Request &,
                                    httplib::Response &res) {
    auto r = execute_query(
        "SELECT binlog_server_reload_user_channel_map() AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("mappings_loaded", r.rows.empty() ? "0" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/rebuild-index
  svr.Post("/api/v1/rebuild-index", [](const httplib::Request &req,
                                       httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    if (ch.empty()) {
      res.status = 400;
      res.set_content(error_json("channel required"), "application/json");
      return;
    }
    auto r = execute_query(
        "SELECT binlog_server_rebuild_archive_index('" + ch + "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("files_processed", r.rows.empty() ? "0" : r.rows[0].values[0]);
    res.set_content(obj.str(), "application/json");
  });

  // PUT /api/v1/variables/:name
  svr.Put("/api/v1/variables/:name", [](const httplib::Request &req,
                                        httplib::Response &res) {
    std::string name = req.path_params.at("name");
    std::string value = parse_json_string(req.body, "value");

    // Only allow binlog_server.* variables
    if (name.find("binlog_server") == std::string::npos) {
      res.status = 403;
      res.set_content(error_json("only binlog_server.* variables allowed"),
                      "application/json");
      return;
    }

    std::string sql = "SET GLOBAL `" + sql_escape(name) + "` = '" +
                      sql_escape(value) + "'";
    auto r = execute_query(sql);
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    json::Object obj;
    obj.add("variable", name);
    obj.add("value", value);
    res.set_content(obj.str(), "application/json");
  });

  // POST /api/v1/search/by-timestamp
  svr.Post("/api/v1/search/by-timestamp", [](const httplib::Request &req,
                                             httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    std::string ts_from = sql_escape(parse_json_string(req.body, "from"));
    std::string ts_to = sql_escape(parse_json_string(req.body, "to"));
    if (ch.empty() || ts_from.empty() || ts_to.empty()) {
      res.status = 400;
      res.set_content(error_json("channel, from, and to required"),
                      "application/json");
      return;
    }
    auto r = execute_query(
        "SELECT binlog_server_search_by_timestamp('" + ch + "', '" + ts_from +
        "', '" + ts_to + "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    std::string json_result =
        (r.rows.empty() || r.rows[0].values.empty()) ? "{}" :
        r.rows[0].values[0];
    res.set_content(json_result, "application/json");
  });

  // POST /api/v1/search/by-gtid-set
  svr.Post("/api/v1/search/by-gtid-set", [](const httplib::Request &req,
                                            httplib::Response &res) {
    std::string ch = sql_escape(parse_json_string(req.body, "channel"));
    std::string gtid = sql_escape(parse_json_string(req.body, "gtid_set"));
    if (ch.empty() || gtid.empty()) {
      res.status = 400;
      res.set_content(error_json("channel and gtid_set required"),
                      "application/json");
      return;
    }
    auto r = execute_query(
        "SELECT binlog_server_search_by_gtid_set('" + ch + "', '" + gtid +
        "') AS result");
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    std::string json_result =
        (r.rows.empty() || r.rows[0].values.empty()) ? "{}" :
        r.rows[0].values[0];
    res.set_content(json_result, "application/json");
  });

  // GET /api/v1/range/:channel
  svr.Get("/api/v1/range/:channel", [](const httplib::Request &req,
                                       httplib::Response &res) {
    std::string ch = sql_escape(req.path_params.at("channel"));
    if (ch.empty()) {
      res.status = 400;
      res.set_content(error_json("channel required"), "application/json");
      return;
    }

    std::string sql =
        "SELECT "
        "DATE_FORMAT(CONVERT_TZ(MIN(MIN_EVENT_TIMESTAMP), "
        "@@session.time_zone, '+00:00'), '%Y-%m-%dT%H:%i:%s') AS min_ts, "
        "DATE_FORMAT(CONVERT_TZ(MAX(MAX_EVENT_TIMESTAMP), "
        "@@session.time_zone, '+00:00'), '%Y-%m-%dT%H:%i:%s') AS max_ts, "
        "COALESCE("
        " (SELECT LAST_GTID_SET"
        "  FROM performance_schema.replication_binlog_server_archive"
        "  WHERE CHANNEL_NAME = '" + ch + "' AND LAST_GTID_SET != ''"
        "  ORDER BY FILE_NAME ASC LIMIT 1),"
        " (SELECT PREVIOUS_GTID_SET"
        "  FROM performance_schema.replication_binlog_server_archive"
        "  WHERE CHANNEL_NAME = '" + ch + "' AND PREVIOUS_GTID_SET != ''"
        "  ORDER BY FILE_NAME ASC LIMIT 1)"
        ") AS first_gtid_set, "
        "COALESCE("
        " (SELECT LAST_GTID_SET"
        "  FROM performance_schema.replication_binlog_server_archive"
        "  WHERE CHANNEL_NAME = '" + ch + "' AND LAST_GTID_SET != ''"
        "  ORDER BY FILE_NAME DESC LIMIT 1),"
        " (SELECT PREVIOUS_GTID_SET"
        "  FROM performance_schema.replication_binlog_server_archive"
        "  WHERE CHANNEL_NAME = '" + ch + "' AND PREVIOUS_GTID_SET != ''"
        "  ORDER BY FILE_NAME DESC LIMIT 1)"
        ") AS last_gtid_set "
        "FROM performance_schema.replication_binlog_server_archive "
        "WHERE CHANNEL_NAME = '" + ch + "'";

    auto r = execute_query(sql);
    if (!r.ok) {
      res.status = 500;
      res.set_content(error_json(r.error), "application/json");
      return;
    }
    if (r.rows.empty()) {
      res.set_content("{\"min_ts\":null,\"max_ts\":null,"
                      "\"first_gtid_set\":\"\",\"last_gtid_set\":\"\"}",
                      "application/json");
      return;
    }

    auto val_or_empty = [&](size_t idx) -> std::string {
      if (idx >= r.rows[0].values.size()) return "";
      const auto &v = r.rows[0].values[idx];
      return (v.empty() || v == "NULL") ? "" : v;
    };

    json::Object obj;
    obj.add("min_ts", val_or_empty(0));
    obj.add("max_ts", val_or_empty(1));
    obj.add("first_gtid_set",
            r.rows[0].values.size() > 2 ? r.rows[0].values[2] : "");
    obj.add("last_gtid_set",
            r.rows[0].values.size() > 3 ? r.rows[0].values[3] : "");
    res.set_content(obj.str(), "application/json");
  });
}

}  // namespace rest_api
