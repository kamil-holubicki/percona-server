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

// --- Streaming I/O implementations ---

namespace {

class FileWriteStream final : public StorageWriteStream {
 public:
  explicit FileWriteStream(const std::string &path)
      : m_path(path), m_fd(-1) {
    m_out.open(path, std::ios::binary | std::ios::out | std::ios::app);
    if (m_out.is_open()) {
      m_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    }
  }
  ~FileWriteStream() override { close(); }

  bool write(const unsigned char *data, size_t len) override {
    m_out.write(reinterpret_cast<const char *>(data),
                static_cast<std::streamsize>(len));
    return m_out.good();
  }
  bool flush() override {
    m_out.flush();
    return m_out.good();
  }
  bool sync() override {
    m_out.flush();
    if (!m_out.good()) return false;
    if (m_fd >= 0) ::fdatasync(m_fd);
    return true;
  }
  void close() override {
    if (m_out.is_open()) {
      m_out.flush();
      m_out.close();
    }
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
  }
  bool good() const override { return m_out.good(); }

 private:
  std::string m_path;
  std::ofstream m_out;
  int m_fd;
};

class FileReadStream final : public StorageReadStream {
 public:
  explicit FileReadStream(const std::string &path) : m_path(path) {
    m_in.open(path, std::ios::binary);
    if (m_in.is_open()) {
      m_in.seekg(0, std::ios::end);
      m_size = static_cast<uint64_t>(m_in.tellg());
    }
  }
  ~FileReadStream() override { close(); }

  bool read_at(uint64_t offset, unsigned char *buf, size_t len) override {
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    m_in.read(reinterpret_cast<char *>(buf),
              static_cast<std::streamsize>(len));
    return m_in.gcount() == static_cast<std::streamsize>(len);
  }
  uint64_t size() const override { return m_size; }
  void close() override {
    if (m_in.is_open()) m_in.close();
  }

 private:
  std::string m_path;
  std::ifstream m_in;
  uint64_t m_size{0};
};

}  // anonymous namespace

std::unique_ptr<StorageWriteStream> FileStorage::open_write(
    const std::string &dir, const std::string &name) {
  auto s = std::make_unique<FileWriteStream>(dir + name);
  if (!s->good()) return nullptr;
  return s;
}

std::unique_ptr<StorageReadStream> FileStorage::open_read(
    const std::string &dir, const std::string &name) const {
  auto s = std::make_unique<FileReadStream>(dir + name);
  if (s->size() == 0 && !fs::exists(dir + name)) return nullptr;
  return s;
}

bool FileStorage::rewrite_header(const std::string &dir,
                                 const std::string &name,
                                 const unsigned char *data, size_t len) {
  std::string path = dir + name;
  FILE *f = std::fopen(path.c_str(), "r+b");
  if (!f) return false;
  size_t written = std::fwrite(data, 1, len, f);
  std::fflush(f);
  std::fclose(f);
  return written == len;
}

bool FileStorage::truncate_file(const std::string &dir,
                                const std::string &name,
                                uint64_t new_size) {
  std::error_code ec;
  fs::resize_file(dir + name, new_size, ec);
  return !ec;
}

}  // namespace binlog_server
