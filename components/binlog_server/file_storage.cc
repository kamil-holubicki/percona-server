/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "file_storage.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace binlog_server {

FileStorage::FileStorage() = default;

const char *FileStorage::type_tag() const { return "file"; }

bool FileStorage::uri_allowed(const char *uri, std::string *reason) const {
  if (uri == nullptr) {
    if (reason != nullptr) *reason = "URI is null";
    return false;
  }
  std::string s(uri);
  if (s.compare(0, 7, "file://") != 0) {
    if (reason != nullptr) *reason = "URI must start with file://";
    return false;
  }
  if (s.size() <= 7) {
    if (reason != nullptr) *reason = "URI path is empty";
    return false;
  }
  return true;
}

std::string FileStorage::resolve_channel_dir(const char *base_uri,
                                             const char *channel_name) const {
  if (base_uri == nullptr) return {};
  std::string path(base_uri);
  if (path.compare(0, 7, "file://") == 0) path = path.substr(7);
  if (!path.empty() && path.back() != '/') path += '/';
  std::string dir_name =
      (channel_name == nullptr || channel_name[0] == '\0') ? "_default"
                                                           : channel_name;
  return path + dir_name + "/";
}

bool FileStorage::ensure_channel_dir(const std::string &channel_dir) {
  std::error_code ec;
  fs::create_directories(channel_dir, ec);
  return !ec;
}

bool FileStorage::wipe_channel_dir(const std::string &channel_dir) {
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(channel_dir, ec)) {
    if (entry.is_regular_file()) {
      std::error_code rm_ec;
      fs::remove(entry.path(), rm_ec);
    }
  }
  return !ec;
}

bool FileStorage::file_exists(const std::string &dir,
                              const std::string &name) const {
  std::error_code ec;
  return fs::exists(dir + name, ec);
}

std::uint64_t FileStorage::file_size(const std::string &dir,
                                     const std::string &name) const {
  std::error_code ec;
  auto sz = fs::file_size(dir + name, ec);
  return ec ? 0 : static_cast<std::uint64_t>(sz);
}

bool FileStorage::remove_file(const std::string &dir,
                              const std::string &name) {
  std::error_code ec;
  return fs::remove(dir + name, ec);
}

std::uint64_t FileStorage::total_bytes(const std::string &dir) const {
  std::uint64_t total = 0;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file()) {
      auto sz = entry.file_size(ec);
      if (!ec) total += sz;
    }
  }
  return total;
}

bool FileStorage::index_load(const std::string &dir,
                             std::vector<std::string> &out) const {
  std::string path = dir + "binlog.index";
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return true;
}

bool FileStorage::index_append(const std::string &dir,
                               const std::string &entry) {
  std::string path = dir + "binlog.index";
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out.is_open()) return false;
  out << entry << "\n";
  out.flush();
  return out.good();
}

bool FileStorage::index_rewrite(const std::string &dir,
                                const std::vector<std::string> &entries) {
  const std::string final_path = dir + "binlog.index";
  const std::string tmp_path = dir + "binlog.index.tmp";

  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const auto &e : entries) {
    out << e << "\n";
  }
  out.flush();
  if (!out.good()) {
    out.close();
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
  }
  out.close();

  // fsync the temp file for durability
  int fd = ::open(tmp_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }

  // Atomic rename
  if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool FileStorage::sidecar_load(const std::string &dir, const std::string &name,
                               std::string &out) const {
  std::string path = dir + name + ".meta";
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool FileStorage::sidecar_store(const std::string &dir, const std::string &name,
                                const std::string &data) {
  const std::string final_path = dir + name + ".meta";
  const std::string tmp_path = dir + name + ".meta.tmp";

  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  out << data;
  out.flush();
  if (!out.good()) {
    out.close();
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
  }
  out.close();

  if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
  }
  return true;
}

}  // namespace binlog_server
