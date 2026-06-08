/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "file_storage.h"

#include <string>

namespace binlog_server {

FileStorage::FileStorage(const std::string &root_path) : m_root(root_path) {
  if (!m_root.empty() && m_root.back() != '/') m_root += '/';
}

bool FileStorage::supports_uri(const std::string &uri) const {
  return uri.compare(0, 7, "file://") == 0;
}

std::string FileStorage::resolve_base_dir(const std::string &uri,
                                          const std::string &channel_name) {
  std::string path = uri.substr(7);  // strip "file://"
  if (!path.empty() && path.back() != '/') path += '/';
  std::string dir_name = channel_name.empty() ? "_default" : channel_name;
  return path + dir_name + "/";
}

}  // namespace binlog_server
