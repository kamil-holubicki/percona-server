/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_FILE_STORAGE_H
#define BINLOG_SERVER_FILE_STORAGE_H

#include "storage_backend.h"

#include <string>

namespace binlog_server {

class FileStorage : public StorageBackend {
 public:
  explicit FileStorage(const std::string &root_path);
  ~FileStorage() override = default;

  bool supports_uri(const std::string &uri) const override;
  std::string resolve_base_dir(const std::string &uri,
                               const std::string &channel_name) override;

 private:
  std::string m_root;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_FILE_STORAGE_H */
