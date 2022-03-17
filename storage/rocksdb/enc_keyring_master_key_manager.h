#pragma once

#include "./enc_master_key_manager.h"
#include "mysql/components/my_service.h"
#include "mysql/components/services/registry.h"

#include <mysql/components/services/keyring_generator.h>
#include <mysql/components/services/keyring_reader_with_status.h>
#include <mysql/components/services/keyring_writer.h>
#include <mysql/service_plugin_registry.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rocksdb {
class Logger;
}

namespace myrocks {
class EncryptionInfoStorage;
class EncryptionUuidProvider;

class KeyringMasterKeyManager : public MasterKeyManager {
 public:
  KeyringMasterKeyManager(
      std::shared_ptr<EncryptionInfoStorage> encInfoStorage,
      std::shared_ptr<EncryptionUuidProvider> encUuidProvider,
      std::function<bool()> checkThdKilledFn,
      std::shared_ptr<rocksdb::Logger> logger);
  ~KeyringMasterKeyManager() override;

  int GetMostRecentMasterKey(std::string *masterKey,
                             uint32_t *masterKeyId) override;
  int GetMasterKey(uint32_t masterKeyId, const std::string &suuid,
                   std::string *masterKey) override;
  void GetServerUuid(std::string *serverUuid) override;
  int GenerateNewMasterKey() override;
  void DisableNewMasterKeyGeneration() override;
  void EnableNewMasterKeyGeneration() override;

  void GetMasterKeyInfo(uint32_t *oldestMasterKeyId,
    uint32_t *newestMasterKeyId, std::string *serverUuid);

 private:
  int GenerateNewMasterKeyUnprotected();
  void InitKeyringServices();
  void DeinitKeyringServices();
  int ReadSecret(const std::string &keyName, std::string *secret);
  int GetSecretFromCache(const std::string &keyName, std::string *secret);
  void StoreSecretInCache(const std::string &keyName,
                          const std::string &secret);
  const std::string CreateKeyName(uint32_t keyId,
                                  const std::string &suuid) const;
  void Recover();

  SERVICE_TYPE(keyring_reader_with_status) * keyring_reader_service_{nullptr};
  SERVICE_TYPE(keyring_writer) * keyring_writer_service_{nullptr};
  SERVICE_TYPE(keyring_generator) * keyring_generator_service_{nullptr};

  uint32_t oldestMasterKeyId_;
  uint32_t newestMasterKeyId_;
  std::string serverUuid_;

  std::map<std::string, std::string> keysCache_;
  std::mutex keysCacheMtx_;
  std::mutex masterKeyIdMutex_;
  std::condition_variable masterKeyIdCv_;

  uint32_t disableNewMasterKeyGenerationCounter_;

  std::shared_ptr<EncryptionInfoStorage> encInfoStorage_;
  std::shared_ptr<EncryptionUuidProvider> encUuidProvider_;
  std::function<bool()> checkThdKilledFn_;
  std::shared_ptr<rocksdb::Logger> logger_;
};

}  // namespace myrocks