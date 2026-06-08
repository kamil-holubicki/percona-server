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

#ifndef COMPONENTS_BINLOG_SERVER_SERVER_SERVICES_H
#define COMPONENTS_BINLOG_SERVER_SERVER_SERVICES_H

#include <atomic>
#include <cstdint>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_binlog_dump_handler_io.h>
#include <mysql/components/services/mysql_binlog_dump_handler_register.h>
#include <mysql/components/services/mysql_thd_kill_handler.h>

extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_binlog_dump_handler_register,
                                       dump_handler_register_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_binlog_dump_handler_io,
                                       dump_handler_io_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_thd_kill_handler,
                                       thd_kill_handler_srv);

namespace binlog_server {
namespace server_services {

inline bool send_event(MYSQL_THD thd, const unsigned char *buf,
                       unsigned long len) {
  return dump_handler_io_srv->send_event(thd, buf, len);
}

inline bool flush(MYSQL_THD thd) {
  return dump_handler_io_srv->flush(thd);
}

class kill_observer {
 public:
  explicit kill_observer(MYSQL_THD thd) : m_thd(thd) {
    (void)thd_kill_handler_srv->set(thd, &kill_observer::on_kill_thunk, this);
  }
  kill_observer(const kill_observer &) = delete;
  kill_observer &operator=(const kill_observer &) = delete;
  ~kill_observer() {
    (void)thd_kill_handler_srv->set(m_thd, nullptr, nullptr);
  }
  bool killed() const { return m_killed.load(std::memory_order_acquire); }

 private:
  static void on_kill_thunk(MYSQL_THD /*thd*/, std::uint16_t /*state*/,
                            void *data) {
    static_cast<kill_observer *>(data)->m_killed.store(
        true, std::memory_order_release);
  }
  MYSQL_THD m_thd;
  std::atomic<bool> m_killed{false};
};

}  // namespace server_services
}  // namespace binlog_server

#endif /* COMPONENTS_BINLOG_SERVER_SERVER_SERVICES_H */
