/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_STORAGE_BACKEND_H
#define BINLOG_SERVER_STORAGE_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace binlog_server {

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual bool supports_uri(const std::string &uri) const = 0;
  virtual std::string resolve_base_dir(const std::string &uri,
                                       const std::string &channel_name) = 0;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_STORAGE_BACKEND_H */
