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

#include "sql/binlog_dump_handler.h"

#include <atomic>

namespace {
std::atomic<Binlog_dump_handler_func> g_binlog_dump_handler{nullptr};
}  // namespace

void set_binlog_dump_handler(Binlog_dump_handler_func handler) {
  g_binlog_dump_handler.store(handler, std::memory_order_release);
}

Binlog_dump_handler_func get_binlog_dump_handler() {
  return g_binlog_dump_handler.load(std::memory_order_acquire);
}
