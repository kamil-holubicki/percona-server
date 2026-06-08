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

#include "sql/server_component/mysql_binlog_dump_handler_imp.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql_com.h"
#include "sql/binlog_dump_handler.h"
#include "sql/item_func.h"
#include "sql/log_event.h"
#include "sql/mysqld.h"
#include "sql/protocol_classic.h"
#include "sql/rpl_gtid.h"
#include "sql/rpl_source.h"
#include "sql/sql_class.h"
#include "typelib.h"

namespace {

std::atomic<mysql_binlog_dump_handler_fn> g_service_handler_fn{nullptr};
std::atomic<void *> g_service_handler_user_data{nullptr};

inline THD *as_thd(MYSQL_THD thd) { return static_cast<THD *>(thd); }

const char *snapshot_user(THD *t) {
  if (t == nullptr) return "";
  const auto *sctx = t->security_context();
  if (sctx == nullptr) return "";
  const LEX_CSTRING u = sctx->user();
  return (u.str != nullptr) ? u.str : "";
}

mysql_binlog_dump_handler_checksum_alg snapshot_checksum(THD *t) {
  if (t == nullptr) return MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_UNDEF;
  mysql_binlog_dump_handler_checksum_alg out =
      MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_UNDEF;
  mysql_mutex_lock(&t->LOCK_thd_data);
  const auto *uv = get_user_var_from_alternatives(
      t, "source_binlog_checksum", "master_binlog_checksum");
  if (uv != nullptr && uv->ptr() != nullptr) {
    const int alg = find_type(uv->ptr(), &binlog_checksum_typelib, 1) - 1;
    switch (alg) {
      case mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF:
        out = MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_OFF;
        break;
      case mysql::binlog::event::BINLOG_CHECKSUM_ALG_CRC32:
        out = MYSQL_BINLOG_DUMP_HANDLER_CHECKSUM_ALG_CRC32;
        break;
      default:
        break;
    }
  }
  mysql_mutex_unlock(&t->LOCK_thd_data);
  return out;
}

std::string snapshot_replica_executed(Gtid_set *gtid_set) {
  if (gtid_set == nullptr || gtid_set->is_empty()) return {};
  const size_t len = gtid_set->get_string_length();
  std::string out;
  out.resize(len);
  if (len > 0) gtid_set->to_string(out.data(), /*need_lock=*/false);
  return out;
}

bool service_handler_bridge(THD *thd, const char *log_ident, my_off_t pos,
                            Gtid_set *gtid_set, uint32 flags) {
  auto fn = g_service_handler_fn.load(std::memory_order_acquire);
  if (fn == nullptr) return false;
  void *user_data =
      g_service_handler_user_data.load(std::memory_order_acquire);

  const std::string replica_gtids = snapshot_replica_executed(gtid_set);
  const char *replica_user = snapshot_user(thd);
  const mysql_binlog_dump_handler_checksum_alg checksum =
      snapshot_checksum(thd);
  const uint32_t source_id = static_cast<uint32_t>(::server_id);
  const uint32_t replica_id =
      (thd == nullptr) ? 0U : static_cast<uint32_t>(thd->server_id);

  return fn(user_data, static_cast<MYSQL_THD>(thd),
            log_ident == nullptr ? "" : log_ident,
            static_cast<uint64_t>(pos), replica_gtids.c_str(), flags,
            source_id, replica_id, checksum,
            replica_user == nullptr ? "" : replica_user);
}

}  // namespace

DEFINE_BOOL_METHOD(mysql_binlog_dump_handler_register_imp::install,
                   (mysql_binlog_dump_handler_fn handler, void *user_data)) {
  if (handler == nullptr) return true;
  mysql_binlog_dump_handler_fn expected = nullptr;
  if (!g_service_handler_fn.compare_exchange_strong(
          expected, handler, std::memory_order_release,
          std::memory_order_acquire)) {
    return true;
  }
  g_service_handler_user_data.store(user_data, std::memory_order_release);
  set_binlog_dump_handler(&service_handler_bridge);
  return false;
}

DEFINE_BOOL_METHOD(mysql_binlog_dump_handler_register_imp::uninstall, ()) {
  set_binlog_dump_handler(nullptr);
  g_service_handler_fn.store(nullptr, std::memory_order_release);
  g_service_handler_user_data.store(nullptr, std::memory_order_release);
  return false;
}

DEFINE_BOOL_METHOD(mysql_binlog_dump_handler_io_imp::send_event,
                   (MYSQL_THD thd, const unsigned char *event_buf,
                    unsigned long event_len)) {
  THD *t = as_thd(thd);
  if (t == nullptr || event_buf == nullptr || event_len == 0) return true;

  std::vector<unsigned char> framed;
  framed.reserve(event_len + 1);
  framed.push_back('\0');
  framed.insert(framed.end(), event_buf, event_buf + event_len);

  NET *net = t->get_protocol_classic()->get_net();
  if (net == nullptr) return true;
  return my_net_write(net, framed.data(), framed.size()) != 0;
}

DEFINE_BOOL_METHOD(mysql_binlog_dump_handler_io_imp::flush, (MYSQL_THD thd)) {
  THD *t = as_thd(thd);
  if (t == nullptr) return true;
  return t->get_protocol()->flush();
}
