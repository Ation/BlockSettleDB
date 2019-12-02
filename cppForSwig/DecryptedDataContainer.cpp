////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2017, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "DecryptedDataContainer.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// DecryptedDataContainer
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::initAfterLock()
{
   auto&& decryptedDataInstance = make_unique<DecryptedDataMaps>();

   //copy default encryption key
   auto&& defaultEncryptionKeyCopy = defaultEncryptionKey_.copy();

   auto defaultKey =
      make_unique<DecryptedEncryptionKey>(defaultEncryptionKeyCopy);
   decryptedDataInstance->encryptionKeys_.insert(make_pair(
      defaultEncryptionKeyId_, move(defaultKey)));

   lockedDecryptedData_ = move(decryptedDataInstance);
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::cleanUpBeforeUnlock()
{
   otherLocks_.clear();
   lockedDecryptedData_.reset();
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::lockOther(
   shared_ptr<DecryptedDataContainer> other)
{
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
      throw DecryptedDataContainerException(
      "nullptr lock! how did we get this far?");

   otherLocks_.push_back(OtherLockedContainer(other));
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<DecryptedEncryptionKey>
DecryptedDataContainer::deriveEncryptionKey(
unique_ptr<DecryptedEncryptionKey> decrKey, const BinaryData& kdfid) const
{
   //sanity check
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
      throw DecryptedDataContainerException(
      "nullptr lock! how did we get this far?");

   //does the decryption key have this derivation?
   auto derivationIter = decrKey->derivedKeys_.find(kdfid);
   if (derivationIter == decrKey->derivedKeys_.end())
   {
      //look for the kdf
      auto kdfIter = kdfMap_.find(kdfid);
      if (kdfIter == kdfMap_.end() || kdfIter->second == nullptr)
         throw DecryptedDataContainerException("can't find kdf params for id");

      //derive the key, this will insert it into the container too
      decrKey->deriveKey(kdfIter->second);
   }

   return move(decrKey);
}

////////////////////////////////////////////////////////////////////////////////
const SecureBinaryData& DecryptedDataContainer::getDecryptedPrivateData(
   const shared_ptr<Asset_EncryptedData>& dataPtr)
{
   return getDecryptedPrivateData(dataPtr.get());
}

////////////////////////////////////////////////////////////////////////////////
const SecureBinaryData& DecryptedDataContainer::getDecryptedPrivateData(
   const Asset_EncryptedData* dataPtr)
{
   //sanity check
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
      throw DecryptedDataContainerException(
      "nullptr lock! how did we get this far?");

   if (dataPtr == nullptr)
      throw DecryptedDataContainerException("null data");

   auto insertDecryptedData = [this](unique_ptr<DecryptedData> decrKey)->
      const SecureBinaryData&
   {
      //if decrKey is empty, all casts failed, throw
      if (decrKey == nullptr)
      throw DecryptedDataContainerException("unexpected dataPtr type");

      //make sure insertion succeeds
      lockedDecryptedData_->privateData_.erase(decrKey->getId());
      auto&& keypair = make_pair(decrKey->getId(), move(decrKey));
      auto&& insertionPair =
         lockedDecryptedData_->privateData_.insert(move(keypair));

      return insertionPair.first->second->getDataRef();
   };

   //look for already decrypted data
   auto dataIter = lockedDecryptedData_->privateData_.find(dataPtr->getId());
   if (dataIter != lockedDecryptedData_->privateData_.end())
      return dataIter->second->getDataRef();

   //no decrypted val entry, let's try to decrypt the data instead

   if (!dataPtr->hasData())
   {
      //missing encrypted data in container (most likely uncomputed private key)
      //throw back to caller, this object only deals with decryption
      throw EncryptedDataMissing();
   }

   //check cipher
   if (!dataPtr->hasData())
   {
      //null cipher, data is not encrypted, create entry and return it
      auto dataCopy = dataPtr->getCipherText();
      auto&& decrKey = make_unique<DecryptedData>(
         dataPtr->getId(), dataCopy);
      return insertDecryptedData(move(decrKey));
   }

   //we have a valid cipher, grab the encryption key
   auto cipherCopy = dataPtr->getCipherDataPtr()->cipher_->getCopy();
   unique_ptr<DecryptedEncryptionKey> decrKey;
   auto& encryptionKeyId = cipherCopy->getEncryptionKeyId();
   auto& kdfId = cipherCopy->getKdfId();

   map<BinaryData, BinaryData> encrKeyMap;
   encrKeyMap.insert(make_pair(encryptionKeyId, kdfId));
   populateEncryptionKey(encrKeyMap);

   auto decrKeyIter =
      lockedDecryptedData_->encryptionKeys_.find(encryptionKeyId);
   if (decrKeyIter == lockedDecryptedData_->encryptionKeys_.end())
      throw DecryptedDataContainerException("could not get encryption key");

   auto derivationKeyIter = decrKeyIter->second->derivedKeys_.find(kdfId);
   if (derivationKeyIter == decrKeyIter->second->derivedKeys_.end())
      throw DecryptedDataContainerException("could not get derived encryption key");

   //decrypt data
   auto decryptedDataPtr = move(dataPtr->decrypt(derivationKeyIter->second));

   //sanity check
   if (decryptedDataPtr == nullptr)
      throw DecryptedDataContainerException("failed to decrypt data");

   //insert the newly decrypted data in the container and return
   return insertDecryptedData(move(decryptedDataPtr));
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::populateEncryptionKey(
   const map<BinaryData, BinaryData>& keyMap)
{
   /*
   This method looks for existing encryption keys in the container. It will 
   return the key decrypted encryption key is present, or populate the 
   container until it cannot find precursors (an encryption key may be 
   encrypted by another encrpytion). At which point, it will prompt the user
   for a passphrase.

   keyMap: <keyId, kdfId> for all eligible key|kdf pairs. These are listed by 
   the encrypted data that you're looking to decrypt
   */

   BinaryData keyId, kdfId;

   //sanity check
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
      throw DecryptedDataContainerException(
      "nullptr lock! how did we get this far?");

   //lambda to insert keys back into the container
   auto insertDecryptedData = [this](
      const BinaryData& keyid, unique_ptr<DecryptedEncryptionKey> decrKey)->void
   {
      //if decrKey is empty, all casts failed, throw
      if (decrKey == nullptr)
         throw DecryptedDataContainerException(
         "tried to insert empty decryption key");

      //make sure insertion succeeds
      lockedDecryptedData_->encryptionKeys_.erase(keyid);
      auto&& keypair = make_pair(keyid, move(decrKey));
      auto&& insertionPair =
         lockedDecryptedData_->encryptionKeys_.insert(move(keypair));
   };

   //look for already decrypted data
   unique_ptr<DecryptedEncryptionKey> decryptedKey = nullptr;

   for (auto& keyPair : keyMap)
   {
      auto dataIter = lockedDecryptedData_->encryptionKeys_.find(keyPair.first);
      if (dataIter != lockedDecryptedData_->encryptionKeys_.end())
      {
         decryptedKey = move(dataIter->second);
         keyId = keyPair.first;
         kdfId = keyPair.second;
         break;
      }
   }

   if (decryptedKey == nullptr)
   {
      //we don't have a decrypted key, let's look for it in the encrypted map
      for (auto& keyPair : keyMap)
      {
         auto encrKeyIter = encryptionKeyMap_.find(keyPair.first);
         if (encrKeyIter != encryptionKeyMap_.end())
         {
            //sanity check
            auto encryptedKeyPtr = dynamic_pointer_cast<Asset_EncryptionKey>(
               encrKeyIter->second);
            if (encryptedKeyPtr == nullptr)
            {
               throw DecryptedDataContainerException(
                  "unexpected object for encryption key id");
            }

            //found the encrypted key, need to decrypt it first
            map<BinaryData, BinaryData> parentKeyMap;
            for (auto& cipherPair : encryptedKeyPtr->cipherData_)
            {
               auto cipherDataPtr = cipherPair.second.get();
               if (cipherDataPtr == nullptr)
                  continue;

               parentKeyMap.insert(make_pair(
                  cipherDataPtr->cipher_->getEncryptionKeyId(), cipherDataPtr->cipher_->getKdfId()));
            }

            populateEncryptionKey(parentKeyMap);

            //grab encryption key from map
            bool done = false;
            for (auto& cipherPair : encryptedKeyPtr->cipherData_)
            {
               auto cipherDataPtr = cipherPair.second.get();
               if (cipherDataPtr == nullptr)
                  continue;

               const auto& encrKeyId = cipherDataPtr->cipher_->getEncryptionKeyId();
               const auto& encrKdfId = cipherDataPtr->cipher_->getKdfId();

               auto decrKeyIter =
                  lockedDecryptedData_->encryptionKeys_.find(encrKeyId);
               if (decrKeyIter == lockedDecryptedData_->encryptionKeys_.end())
                  continue;
               auto&& decryptionKey = move(decrKeyIter->second);

               //derive encryption key
               decryptionKey = move(deriveEncryptionKey(move(decryptionKey), encrKdfId));

               //decrypt encrypted key
               auto&& rawDecryptedKey = cipherDataPtr->cipher_->decrypt(
                  decryptionKey->getDerivedKey(encrKdfId),
                  cipherDataPtr->cipherText_);

               decryptedKey = move(make_unique<DecryptedEncryptionKey>(
                  rawDecryptedKey));

               //move decryption key back to container
               insertDecryptedData(encrKeyId, move(decryptionKey));
               done = true;
            }

            if(!done)
               throw DecryptedDataContainerException("failed to decrypt key");

            keyId = keyPair.first;
            kdfId = keyPair.second;

            break;
         }
      }
   }

   if (decryptedKey == nullptr)
   {
      //still no key, prompt the user
      decryptedKey = move(promptPassphrase(keyMap));
      for (auto& keyPair : keyMap)
      {
         if (decryptedKey->getId(keyPair.second) == keyPair.first)
         {
            keyId = keyPair.first;
            kdfId = keyPair.second;
            break;
         }
      }
   }

   //apply kdf
   decryptedKey = move(deriveEncryptionKey(move(decryptedKey), kdfId));

   //insert into map
   insertDecryptedData(keyId, move(decryptedKey));
}

////////////////////////////////////////////////////////////////////////////////
SecureBinaryData DecryptedDataContainer::encryptData(
   Cipher* const cipher, const SecureBinaryData& data)
{
   //sanity check
   if (cipher == nullptr)
      throw DecryptedDataContainerException("null cipher");

   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
      throw DecryptedDataContainerException(
      "nullptr lock! how did we get this far?");

   map<BinaryData, BinaryData> keyMap;
   keyMap.insert(make_pair(cipher->getEncryptionKeyId(), cipher->getKdfId()));
   populateEncryptionKey(keyMap);

   auto keyIter = lockedDecryptedData_->encryptionKeys_.find(
      cipher->getEncryptionKeyId());
   auto& derivedKey = keyIter->second->getDerivedKey(cipher->getKdfId());

   return move(cipher->encrypt(
      keyIter->second.get(), cipher->getKdfId(), data));
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<DecryptedEncryptionKey> DecryptedDataContainer::promptPassphrase(
   const map<BinaryData, BinaryData>& keyMap) const
{
   while (1)
   {
      if (!getPassphraseLambda_)
         throw DecryptedDataContainerException("empty passphrase lambda");

      set<BinaryData> keySet;
      for (auto& keyPair : keyMap)
         keySet.insert(keyPair.first);

      auto&& passphrase = getPassphraseLambda_(keySet);

      if (passphrase.getSize() == 0)
         throw DecryptedDataContainerException("empty passphrase");

      auto keyPtr = make_unique<DecryptedEncryptionKey>(passphrase);
      for (auto& keyPair : keyMap)
      {
         keyPtr = move(deriveEncryptionKey(move(keyPtr), keyPair.second));

         if (keyPair.first == keyPtr->getId(keyPair.second))
            return move(keyPtr);
      }
   }

   return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::updateKeyOnDisk(
   const BinaryData& key, shared_ptr<Asset_EncryptedData> dataPtr)
{
   //serialize db key
   auto&& dbKey = WRITE_UINT8_BE(ENCRYPTIONKEY_PREFIX);
   dbKey.append(key);

   updateKeyOnDiskNoPrefix(dbKey, dataPtr);
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::updateKeyOnDiskNoPrefix(
   const BinaryData& dbKey, shared_ptr<Asset_EncryptedData> dataPtr)
{
   auto&& tx = iface_->beginWriteTransaction(dbName_);

   //check if data is on disk already
   auto&& dataRef = tx->getDataRef(dbKey);

   if (dataRef.getSize() != 0)
   {
      //already have this key, is it the same data?
      auto onDiskData = Asset_EncryptedData::deserialize(dataRef);

      //data has not changed, no need to commit
      if (onDiskData->isSame(dataPtr.get()))
         return;

      //data has changed, wipe the existing data
      deleteKeyFromDisk(dbKey);
   }

   auto&& serializedData = dataPtr->serialize();
   tx->insert(dbKey, serializedData);
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::updateOnDisk()
{
   auto&& tx = iface_->beginWriteTransaction(dbName_);

   //encryption keys
   for (auto& key : encryptionKeyMap_)
      updateKeyOnDisk(key.first, key.second);

   //kdf
   for (auto& key : kdfMap_)
   {
      //get db key
      auto&& dbKey = WRITE_UINT8_BE(KDF_PREFIX);
      dbKey.append(key.first);

      //fetch from db
      auto&& dataRef = tx->getDataRef(dbKey);

      if (dataRef.getSize() != 0)
      {
         //already have this key, is it the same data?
         auto onDiskData = KeyDerivationFunction::deserialize(dataRef);

         //data has not changed, not commiting to disk
         if (onDiskData->isSame(key.second.get()))
            continue;

         //data has changed, wipe the existing data
         deleteKeyFromDisk(dbKey);
      }

      auto&& serializedData = key.second->serialize();
      tx->insert(dbKey, serializedData);
   }
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::deleteKeyFromDisk(const BinaryData& key)
{
   //sanity checks
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   //erase key, db interface will wipe it from file
   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->erase(key);
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::readFromDisk()
{
   auto&& tx = iface_->beginReadTransaction(dbName_);
   readFromDisk(tx.get());
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::readFromDisk(DBIfaceTransaction* tx)
{
   //encryption key and kdf entries
   auto dbIter = tx->getIterator();

   BinaryWriter bwEncrKey;
   bwEncrKey.put_uint8_t(ENCRYPTIONKEY_PREFIX);
   dbIter->seek(bwEncrKey.getData());

   while (dbIter->isValid())
   {
      auto iterkey = dbIter->key();
      auto itervalue = dbIter->value();

      if (iterkey.getSize() < 2)
         throw runtime_error("empty db key");

      if (itervalue.getSize() < 1)
         throw runtime_error("empty value");

      auto prefix = (uint8_t*)iterkey.getPtr();
      switch (*prefix)
      {
      case ENCRYPTIONKEY_PREFIX:
      {
         auto keyUPtr = Asset_EncryptedData::deserialize(itervalue);
         shared_ptr<Asset_EncryptedData> keySPtr(move(keyUPtr));
         auto encrKeyPtr = dynamic_pointer_cast<Asset_EncryptionKey>(keySPtr);
         if (encrKeyPtr == nullptr)
            throw runtime_error("empty keyptr");

         addEncryptionKey(encrKeyPtr);

         break;
      }

      case KDF_PREFIX:
      {
         auto kdfPtr = KeyDerivationFunction::deserialize(itervalue);
         if (iterkey.getSliceRef(1, iterkey.getSize() - 1) != kdfPtr->getId())
            throw runtime_error("kdf id mismatch");

         addKdf(kdfPtr);
         break;
      }
      }

      dbIter->advance();
   }
}

////////////////////////////////////////////////////////////////////////////////
void DecryptedDataContainer::encryptEncryptionKey(
   const BinaryData& keyID, const BinaryData& kdfID,
   const SecureBinaryData& newPassphrase, bool replace)
{
   /***
   Encrypts an encryption key with newPassphrase. 
   
   Will swap old passphrase with new one if replace is true.
   Will add the passphrase to the designated key if replace is false.

   The code detects which passphrase was used to decrypt the key prior to 
   adding the new passphrase. For this purpose it needs to control the lifespan
   of the encryption lock.
   
   Pre-existing locks may have the relevant key already decrypted, and the 
   passphrase that was used to decrypt it with will be replaced, which may not
   reflect the user's intent.

   Therefor, this method tries to SingleLock itself, and will fail if a lock is
   held elsewhere, even within the same thread.
   ***/

   SingleLock lock(this);

   //sanity check
   if (!ownsLock())
      throw DecryptedDataContainerException("unlocked/does not own lock");

   if (lockedDecryptedData_ == nullptr)
   {
      throw DecryptedDataContainerException(
         "nullptr lock! how did we get this far?");
   }

   //grab encryption key object
   auto keyIter = encryptionKeyMap_.find(keyID);
   if (keyIter == encryptionKeyMap_.end())
   {
      throw DecryptedDataContainerException(
         "cannot change passphrase for unknown key");
   }
   auto encryptedKey = dynamic_pointer_cast<Asset_EncryptionKey>(keyIter->second);

   //decrypt master encryption key
   map<BinaryData, BinaryData> encrKeyMap;
   encrKeyMap.insert(make_pair(keyID, kdfID));
   populateEncryptionKey(encrKeyMap);

   //grab decrypted key
   auto decryptedKeyIter = lockedDecryptedData_->encryptionKeys_.find(keyID);
   if (decryptedKeyIter == lockedDecryptedData_->encryptionKeys_.end())
      throw DecryptedDataContainerException("failed to decrypt key");
   auto& decryptedKey = decryptedKeyIter->second->getData();

   //grab kdf for key id computation
   auto kdfIter = kdfMap_.find(kdfID);
   if (kdfIter == kdfMap_.end())
      throw DecryptedDataContainerException("failed to grab kdf");

   //copy passphrase cause the ctor will move the data in
   auto newPassphraseCopy = newPassphrase;

   //kdf the key to get its id
   auto newEncryptionKey = make_unique<DecryptedEncryptionKey>(newPassphraseCopy);
   newEncryptionKey->deriveKey(kdfIter->second);
   auto newKeyId = newEncryptionKey->getId(kdfID);

   //TODO: figure out which passphrase unlocked the key
   Cipher* cipherPtr = nullptr;
   for (auto& keyPair : lockedDecryptedData_->encryptionKeys_)
   {
       cipherPtr = encryptedKey->getCipherPtrForId(keyPair.first);
       if (cipherPtr != nullptr)
          break;
   }

   if (cipherPtr == nullptr)
      throw DecryptedDataContainerException("failed to find encryption key");

   //create new cipher, pointing to the new key id
   auto newCipher = cipherPtr->getCopy(newKeyId);

   //add new encryption key object to container
   lockedDecryptedData_->encryptionKeys_.insert(
      move(make_pair(newKeyId, move(newEncryptionKey))));

   //encrypt master key
   auto&& newEncryptedKey = encryptData(newCipher.get(), decryptedKey);

   //create new encrypted container
   auto newCipherData = make_unique<CipherData>(newEncryptedKey, move(newCipher));

   if (replace)
   {
      //remove old cipher data from the encrypted key object
      if (!encryptedKey->removeCipherData(cipherPtr->getEncryptionKeyId()))
         throw DecryptedDataContainerException("failed to erase old encryption key");
   }

   //add new cipher data object to the encrypted key object
   if (!encryptedKey->addCipherData(move(newCipherData)))
      throw DecryptedDataContainerException("cipher data already present in encryption key");

   auto&& temp_key = WRITE_UINT8_BE(ENCRYPTIONKEY_PREFIX_TEMP);
   temp_key.append(keyID);
   auto&& perm_key = WRITE_UINT8_BE(ENCRYPTIONKEY_PREFIX);
   perm_key.append(keyID);

   {
      //write new encrypted key as temp key within it's own transaction
      auto&& tx = iface_->beginWriteTransaction(dbName_);
      updateKeyOnDiskNoPrefix(temp_key, encryptedKey);
   }

   {
      auto&& tx = iface_->beginWriteTransaction(dbName_);

      //wipe old key from disk
      deleteKeyFromDisk(perm_key);

      //write new key to disk
      updateKeyOnDiskNoPrefix(perm_key, encryptedKey);
   }

   {
      //wipe temp entry
      auto&& tx = iface_->beginWriteTransaction(dbName_);
      deleteKeyFromDisk(temp_key);
   }
}
