/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "s3_storage.h"
#include "log_helpers.h"

#include <memory>
#include <string>

namespace binlog_server {

namespace {
void log_not_implemented(const char *method) {
  bslog(ERROR_LEVEL,
        "binlog_server: S3 storage backend is not yet implemented "
        "(called %s). Use file:// storage URIs for now.",
        method);
}
}  // namespace

S3Storage::S3Storage() = default;
S3Storage::~S3Storage() = default;

const char *S3Storage::type_tag() const { return "s3"; }

bool S3Storage::uri_allowed(const char *uri, std::string *reason) const {
  if (uri == nullptr) {
    if (reason) *reason = "URI is null";
    return false;
  }
  std::string s(uri);
  if (s.compare(0, 5, "s3://") != 0) {
    if (reason) *reason = "URI must start with s3://";
    return false;
  }
  if (reason) *reason = "S3 storage backend is not yet implemented";
  return false;
}

std::string S3Storage::resolve_channel_dir(const char *base_uri,
                                           const char *channel_name) const {
  (void)base_uri;
  (void)channel_name;
  log_not_implemented("resolve_channel_dir");
  return {};
}

bool S3Storage::ensure_channel_dir(const std::string &channel_dir) {
  (void)channel_dir;
  log_not_implemented("ensure_channel_dir");
  return false;
}

bool S3Storage::wipe_channel_dir(const std::string &channel_dir) {
  (void)channel_dir;
  log_not_implemented("wipe_channel_dir");
  return false;
}

bool S3Storage::file_exists(const std::string &dir,
                            const std::string &name) const {
  (void)dir;
  (void)name;
  log_not_implemented("file_exists");
  return false;
}

std::uint64_t S3Storage::file_size(const std::string &dir,
                                   const std::string &name) const {
  (void)dir;
  (void)name;
  log_not_implemented("file_size");
  return 0;
}

bool S3Storage::remove_file(const std::string &dir, const std::string &name) {
  (void)dir;
  (void)name;
  log_not_implemented("remove_file");
  return false;
}

std::uint64_t S3Storage::total_bytes(const std::string &dir) const {
  (void)dir;
  log_not_implemented("total_bytes");
  return 0;
}

bool S3Storage::index_load(const std::string &dir,
                           std::vector<std::string> &out) const {
  (void)dir;
  (void)out;
  log_not_implemented("index_load");
  return false;
}

bool S3Storage::index_append(const std::string &dir,
                             const std::string &entry) {
  (void)dir;
  (void)entry;
  log_not_implemented("index_append");
  return false;
}

bool S3Storage::index_rewrite(const std::string &dir,
                              const std::vector<std::string> &entries) {
  (void)dir;
  (void)entries;
  log_not_implemented("index_rewrite");
  return false;
}

bool S3Storage::sidecar_load(const std::string &dir, const std::string &name,
                             std::string &out) const {
  (void)dir;
  (void)name;
  (void)out;
  log_not_implemented("sidecar_load");
  return false;
}

bool S3Storage::sidecar_store(const std::string &dir, const std::string &name,
                              const std::string &data) {
  (void)dir;
  (void)name;
  (void)data;
  log_not_implemented("sidecar_store");
  return false;
}

std::unique_ptr<StorageWriteStream> S3Storage::open_write(
    const std::string &dir, const std::string &name) {
  (void)dir;
  (void)name;
  log_not_implemented("open_write");
  return nullptr;
}

std::unique_ptr<StorageReadStream> S3Storage::open_read(
    const std::string &dir, const std::string &name) const {
  (void)dir;
  (void)name;
  log_not_implemented("open_read");
  return nullptr;
}

bool S3Storage::rewrite_header(const std::string &dir, const std::string &name,
                               const unsigned char *data, size_t len) {
  (void)dir;
  (void)name;
  (void)data;
  (void)len;
  log_not_implemented("rewrite_header");
  return false;
}

bool S3Storage::truncate_file(const std::string &dir, const std::string &name,
                              uint64_t new_size) {
  (void)dir;
  (void)name;
  (void)new_size;
  log_not_implemented("truncate_file");
  return false;
}

std::unique_ptr<StorageBackend> create_s3_storage() {
  return std::make_unique<S3Storage>();
}

}  // namespace binlog_server
