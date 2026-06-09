/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "encryption.h"
#include "log_helpers.h"
#include "storage_backend.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <mysql/components/component_implementation.h>
#include <mysql/components/my_service.h>
#include <mysql/components/services/keyring_aes.h>
#include <mysql/components/services/keyring_generator.h>
#include <mysql/components/services/keyring_reader_with_status.h>
#include <mysql/components/services/keyring_writer.h>
#include <mysql/components/services/registry.h>

#include <cstring>

extern REQUIRES_SERVICE_PLACEHOLDER(registry);

namespace binlog_server {

static inline SERVICE_TYPE(registry) *get_registry() {
  return mysql_service_registry;
}

// --- AES-256-CTR Stream Cipher ---

AesCtrCipher::~AesCtrCipher() {
  if (m_ctx) {
    EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX *>(m_ctx));
    m_ctx = nullptr;
  }
  OPENSSL_cleanse(m_file_key, sizeof(m_file_key));
  OPENSSL_cleanse(m_base_iv, sizeof(m_base_iv));
}

bool AesCtrCipher::open(const unsigned char *password, size_t password_len) {
  if (EVP_BytesToKey(EVP_aes_256_ctr(), EVP_sha512(), nullptr, password,
                     static_cast<int>(password_len), 1, m_file_key,
                     m_base_iv) == 0)
    return false;
  m_opened = true;
  m_offset = 0;
  return init_cipher(0);
}

bool AesCtrCipher::init_cipher(uint64_t offset) {
  if (!m_opened) return false;

  if (m_ctx) {
    EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX *>(m_ctx));
    m_ctx = nullptr;
  }

  auto *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;

  // Compute counter from offset
  std::memcpy(m_iv, m_base_iv, kFileIvLen);
  uint64_t counter = offset / kAesBlockSize;
  // Store counter in big-endian in last 8 bytes of IV
  for (int i = 7; i >= 0; --i) {
    m_iv[8 + i] = static_cast<unsigned char>(counter & 0xFF);
    counter >>= 8;
  }

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, m_file_key,
                         m_iv) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  // If offset is not block-aligned, consume partial block
  unsigned partial = static_cast<unsigned>(offset % kAesBlockSize);
  if (partial > 0) {
    unsigned char dummy[kAesBlockSize];
    std::memset(dummy, 0, sizeof(dummy));
    int outlen = 0;
    EVP_EncryptUpdate(ctx, dummy, &outlen, dummy, partial);
  }

  m_ctx = ctx;
  m_offset = offset;
  return true;
}

bool AesCtrCipher::process(unsigned char *data, size_t len) {
  if (!m_ctx) return false;
  auto *ctx = static_cast<EVP_CIPHER_CTX *>(m_ctx);
  int outlen = 0;
  if (EVP_EncryptUpdate(ctx, data, &outlen, data, static_cast<int>(len)) != 1)
    return false;
  m_offset += len;
  return true;
}

bool AesCtrCipher::set_offset(uint64_t offset) {
  return init_cipher(offset);
}

// --- Encryption Header ---

// TLV type constants (MySQL-compatible)
constexpr unsigned char kTlvTypeKeyId = 1;
constexpr unsigned char kTlvTypeEncryptedPassword = 2;
constexpr unsigned char kTlvTypeIv = 3;
constexpr unsigned char kHeaderVersion = 1;

bool EncryptionHeader::is_encrypted(const unsigned char *buf) {
  return std::memcmp(buf, kEncryptionMagic, 4) == 0;
}

bool EncryptionHeader::serialize(unsigned char *buf) const {
  std::memset(buf, 0, kEncryptionHeaderSize);

  // Magic
  std::memcpy(buf, kEncryptionMagic, 4);
  buf[4] = kHeaderVersion;

  unsigned pos = 5;

  // TLV: Key ID (type=1, length=1 byte, value=string)
  if (key_id.size() > 200) return false;
  buf[pos++] = kTlvTypeKeyId;
  buf[pos++] = static_cast<unsigned char>(key_id.size());
  std::memcpy(buf + pos, key_id.data(), key_id.size());
  pos += static_cast<unsigned>(key_id.size());

  // TLV: Encrypted password (type=2, implicit length=32)
  buf[pos++] = kTlvTypeEncryptedPassword;
  std::memcpy(buf + pos, encrypted_password, kFilePasswordLen);
  pos += kFilePasswordLen;

  // TLV: IV (type=3, implicit length=16)
  buf[pos++] = kTlvTypeIv;
  std::memcpy(buf + pos, iv, kFileIvLen);
  pos += kFileIvLen;

  if (pos > kEncryptionHeaderSize) return false;
  return true;
}

bool EncryptionHeader::deserialize(const unsigned char *buf) {
  if (std::memcmp(buf, kEncryptionMagic, 4) != 0) return false;
  if (buf[4] != kHeaderVersion) return false;

  unsigned pos = 5;
  bool got_key_id = false, got_password = false, got_iv = false;

  while (pos < kEncryptionHeaderSize) {
    unsigned char type = buf[pos++];
    if (type == 0) break;  // padding

    switch (type) {
      case kTlvTypeKeyId: {
        if (pos >= kEncryptionHeaderSize) return false;
        unsigned char len = buf[pos++];
        if (pos + len > kEncryptionHeaderSize) return false;
        key_id.assign(reinterpret_cast<const char *>(buf + pos), len);
        pos += len;
        got_key_id = true;
        break;
      }
      case kTlvTypeEncryptedPassword:
        if (pos + kFilePasswordLen > kEncryptionHeaderSize) return false;
        std::memcpy(encrypted_password, buf + pos, kFilePasswordLen);
        pos += kFilePasswordLen;
        got_password = true;
        break;
      case kTlvTypeIv:
        if (pos + kFileIvLen > kEncryptionHeaderSize) return false;
        std::memcpy(iv, buf + pos, kFileIvLen);
        pos += kFileIvLen;
        got_iv = true;
        break;
      default:
        return false;
    }
  }

  return got_key_id && got_password && got_iv;
}

// --- Keyring Integration ---

static const char *kKeyIdPrefix = "BinlogServerKey";
static const char *kSeqnoKeySuffix = "_seqno";

bool encryption_keyring_init() { return get_registry() != nullptr; }

void encryption_keyring_deinit() {}

bool encryption_keyring_available() {
  auto *reg = get_registry();
  if (!reg) return false;
  my_service<SERVICE_TYPE(keyring_aes)> aes("keyring_aes", reg);
  return aes.is_valid();
}

// Build key ID: "BinlogServerKey_{channel}_{seqno}"
static std::string make_key_id(const std::string &channel, uint32_t seqno) {
  return std::string(kKeyIdPrefix) + "_" + channel + "_" +
         std::to_string(seqno);
}

// Build seqno metadata key ID: "BinlogServerKey_{channel}_seqno"
static std::string make_seqno_key_id(const std::string &channel) {
  return std::string(kKeyIdPrefix) + "_" + channel + kSeqnoKeySuffix;
}

// Read current seqno from keyring (stored as 4-byte LE in a SECRET key)
static bool read_seqno(const std::string &channel, uint32_t &seqno) {
  my_service<SERVICE_TYPE(keyring_reader_with_status)> reader(
      "keyring_reader_with_status", get_registry());
  if (!reader.is_valid()) return false;

  std::string seqno_key = make_seqno_key_id(channel);
  my_h_keyring_reader_object reader_obj = nullptr;
  if (reader->init(seqno_key.c_str(), "", &reader_obj))
    return false;
  if (reader_obj == nullptr) {
    seqno = 0;
    return true;
  }

  size_t data_size = 0, type_size = 0;
  reader->fetch_length(reader_obj, &data_size, &type_size);
  if (data_size < 4) {
    reader->deinit(reader_obj);
    return false;
  }

  unsigned char buf[4];
  size_t out_size = 0;
  char type_buf[32];
  size_t out_type_size = 0;
  if (reader->fetch(reader_obj, buf, 4, &out_size, type_buf, sizeof(type_buf),
                    &out_type_size)) {
    reader->deinit(reader_obj);
    return false;
  }
  reader->deinit(reader_obj);

  seqno = static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
  return true;
}

// Write seqno to keyring (remove-then-store since store cannot overwrite)
static bool write_seqno(const std::string &channel, uint32_t seqno) {
  my_service<SERVICE_TYPE(keyring_writer)> writer("keyring_writer",
                                                  get_registry());
  if (!writer.is_valid()) return false;

  std::string seqno_key = make_seqno_key_id(channel);
  unsigned char buf[4];
  buf[0] = static_cast<unsigned char>(seqno & 0xFF);
  buf[1] = static_cast<unsigned char>((seqno >> 8) & 0xFF);
  buf[2] = static_cast<unsigned char>((seqno >> 16) & 0xFF);
  buf[3] = static_cast<unsigned char>((seqno >> 24) & 0xFF);

  // Remove existing key first (ignore error if it doesn't exist yet)
  writer->remove(seqno_key.c_str(), "");
  return !writer->store(seqno_key.c_str(), "", buf, 4, "SECRET");
}

std::string generate_master_key(const std::string &channel) {
  my_service<SERVICE_TYPE(keyring_generator)> gen("keyring_generator",
                                                  get_registry());
  if (!gen.is_valid()) return {};

  uint32_t seqno = 0;
  if (!read_seqno(channel, seqno)) return {};
  uint32_t new_seqno = seqno + 1;

  std::string key_id = make_key_id(channel, new_seqno);
  if (gen->generate(key_id.c_str(), "", "AES", 32)) return {};

  if (!write_seqno(channel, new_seqno)) return {};

  return key_id;
}

std::string current_master_key_id(const std::string &channel) {
  uint32_t seqno = 0;
  if (!read_seqno(channel, seqno)) return {};
  if (seqno == 0) return {};
  return make_key_id(channel, seqno);
}

bool encrypt_file_password(const std::string &key_id,
                           const unsigned char *password,
                           unsigned char *encrypted_out,
                           unsigned char *iv_out) {
  my_service<SERVICE_TYPE(keyring_aes)> aes("keyring_aes", get_registry());
  if (!aes.is_valid()) return false;

  if (RAND_bytes(iv_out, kFileIvLen) != 1) return false;

  // The keyring_aes service internally validates output buffer against
  // get_ciphertext_size() which includes padding overhead.  Use a temp
  // buffer large enough (input + one AES block) to satisfy that check.
  constexpr size_t kBufSize = kFilePasswordLen + 16;
  unsigned char tmp[kBufSize];
  size_t out_len = 0;
  if (aes->encrypt(key_id.c_str(), "", "cbc", 256, iv_out,
                   0 /* no padding */,
                   password, kFilePasswordLen, tmp, kBufSize, &out_len))
    return false;

  if (out_len != kFilePasswordLen) return false;
  std::memcpy(encrypted_out, tmp, kFilePasswordLen);
  OPENSSL_cleanse(tmp, sizeof(tmp));
  return true;
}

bool decrypt_file_password(const std::string &key_id,
                           const unsigned char *encrypted,
                           const unsigned char *iv,
                           unsigned char *password_out) {
  my_service<SERVICE_TYPE(keyring_aes)> aes("keyring_aes", get_registry());
  if (!aes.is_valid()) return false;

  constexpr size_t kBufSize = kFilePasswordLen + 16;
  unsigned char tmp[kBufSize];
  size_t out_len = 0;
  if (aes->decrypt(key_id.c_str(), "", "cbc", 256, iv,
                   0 /* no padding */,
                   encrypted, kFilePasswordLen, tmp, kBufSize, &out_len))
    return false;

  if (out_len != kFilePasswordLen) return false;
  std::memcpy(password_out, tmp, kFilePasswordLen);
  OPENSSL_cleanse(tmp, sizeof(tmp));
  return true;
}

std::string rotate_master_key(const std::string &channel,
                              StorageBackend *backend,
                              const std::string &dir,
                              const std::string &file_name) {
  if (!backend || file_name.empty()) return {};

  auto reader = backend->open_read(dir, file_name);
  if (!reader) return {};

  unsigned char hdr_buf[kEncryptionHeaderSize];
  if (!reader->read_at(0, hdr_buf, kEncryptionHeaderSize)) return {};
  reader->close();

  if (!EncryptionHeader::is_encrypted(hdr_buf)) return {};

  EncryptionHeader hdr;
  if (!hdr.deserialize(hdr_buf)) return {};

  unsigned char file_password[kFilePasswordLen];
  if (!decrypt_file_password(hdr.key_id, hdr.encrypted_password, hdr.iv,
                             file_password)) {
    OPENSSL_cleanse(file_password, sizeof(file_password));
    return {};
  }

  std::string new_key_id = generate_master_key(channel);
  if (new_key_id.empty()) {
    OPENSSL_cleanse(file_password, sizeof(file_password));
    return {};
  }

  unsigned char new_encrypted[kFilePasswordLen];
  unsigned char new_iv[kFileIvLen];
  if (!encrypt_file_password(new_key_id, file_password, new_encrypted,
                             new_iv)) {
    OPENSSL_cleanse(file_password, sizeof(file_password));
    return {};
  }
  OPENSSL_cleanse(file_password, sizeof(file_password));

  EncryptionHeader new_hdr;
  new_hdr.key_id = new_key_id;
  std::memcpy(new_hdr.encrypted_password, new_encrypted, kFilePasswordLen);
  std::memcpy(new_hdr.iv, new_iv, kFileIvLen);

  unsigned char new_hdr_buf[kEncryptionHeaderSize];
  if (!new_hdr.serialize(new_hdr_buf)) return {};

  if (!backend->rewrite_header(dir, file_name, new_hdr_buf,
                               kEncryptionHeaderSize)) {
    return {};
  }

  bslog(INFORMATION_LEVEL,
        "binlog_server: encryption key rotated for channel '%s': "
        "old_key='%s' new_key='%s'",
        channel.c_str(), hdr.key_id.c_str(), new_key_id.c_str());
  return new_key_id;
}

}  // namespace binlog_server
