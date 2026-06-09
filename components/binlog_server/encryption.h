/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_ENCRYPTION_H
#define BINLOG_SERVER_ENCRYPTION_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace binlog_server {

constexpr char kEncryptionMagic[4] = {'\xfd', '\x62', '\x69', '\x6e'};
constexpr unsigned kEncryptionHeaderSize = 512;
constexpr unsigned kFilePasswordLen = 32;
constexpr unsigned kFileKeyLen = 32;
constexpr unsigned kFileIvLen = 16;
constexpr unsigned kAesBlockSize = 16;

// Lightweight AES-256-CTR stream cipher for file body encryption.
// Uses OpenSSL EVP directly. Supports random-access set_offset().
class AesCtrCipher {
 public:
  AesCtrCipher() = default;
  ~AesCtrCipher();

  AesCtrCipher(const AesCtrCipher &) = delete;
  AesCtrCipher &operator=(const AesCtrCipher &) = delete;
  AesCtrCipher(AesCtrCipher &&) = delete;
  AesCtrCipher &operator=(AesCtrCipher &&) = delete;

  // Derive file key + IV from password via EVP_BytesToKey(SHA-512).
  // Must be called before encrypt/decrypt.
  bool open(const unsigned char *password, size_t password_len);

  // Encrypt or decrypt len bytes in-place. CTR mode is symmetric.
  bool process(unsigned char *data, size_t len);

  // Set the stream offset for random-access operations.
  bool set_offset(uint64_t offset);

 private:
  bool init_cipher(uint64_t offset);

  unsigned char m_file_key[kFileKeyLen]{};
  unsigned char m_base_iv[kFileIvLen]{};
  unsigned char m_iv[kFileIvLen]{};
  void *m_ctx{nullptr};  // EVP_CIPHER_CTX*
  uint64_t m_offset{0};
  bool m_opened{false};
};

// Encrypted file header (512 bytes, MySQL-compatible TLV format).
struct EncryptionHeader {
  std::string key_id;
  unsigned char encrypted_password[kFilePasswordLen]{};
  unsigned char iv[kFileIvLen]{};

  // Serialize to 512-byte buffer.
  bool serialize(unsigned char *buf) const;

  // Deserialize from 512-byte buffer (after magic+version check).
  bool deserialize(const unsigned char *buf);

  // Check if buffer starts with encryption magic.
  static bool is_encrypted(const unsigned char *buf);
};

// Registry handle for keyring services (opaque, set at init time).
struct KeyringHandle;

// Initialize keyring service handles (call once at component init).
bool encryption_keyring_init();
void encryption_keyring_deinit();
bool encryption_keyring_available();

// Generate a new master key in the keyring. Returns the key_id.
// Increments the seqno for the channel.
std::string generate_master_key(const std::string &channel);

// Get the current master key ID for a channel (reads seqno from keyring).
std::string current_master_key_id(const std::string &channel);

// Encrypt a file password using the master key (via keyring_aes CBC).
bool encrypt_file_password(const std::string &key_id,
                           const unsigned char *password,
                           unsigned char *encrypted_out,
                           unsigned char *iv_out);

// Decrypt a file password using the master key (via keyring_aes CBC).
bool decrypt_file_password(const std::string &key_id,
                           const unsigned char *encrypted,
                           const unsigned char *iv,
                           unsigned char *password_out);

class StorageBackend;  // forward declaration

// Rotate master key for a channel: generate new key, re-wrap active file.
// Returns new key_id or empty on failure.
std::string rotate_master_key(const std::string &channel,
                              StorageBackend *backend,
                              const std::string &dir,
                              const std::string &file_name);

}  // namespace binlog_server

#endif /* BINLOG_SERVER_ENCRYPTION_H */
