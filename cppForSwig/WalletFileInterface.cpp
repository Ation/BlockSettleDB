////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2019, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "WalletFileInterface.h"
#include "BtcUtils.h"
#include "DBUtils.h"
#include "WalletHeader.h"
#include "DecryptedDataContainer.h"
#include "Assets.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// DBInterface
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
const BinaryData DBInterface::erasurePlaceHolder_ =
BinaryData(ERASURE_PLACE_HOLDER);

const BinaryData DBInterface::keyCycleFlag_ =
BinaryData(KEY_CYCLE_FLAG);

////////////////////////////////////////////////////////////////////////////////
DBInterface::DBInterface(
   LMDBEnv* dbEnv, const std::string& dbName,
   const SecureBinaryData& controlSalt) :
   dbEnv_(dbEnv), dbName_(dbName), controlSalt_(controlSalt)
{
   auto tx = LMDBEnv::Transaction(dbEnv_, LMDB::ReadWrite);
   db_.open(dbEnv_, dbName_);
}

////////////////////////////////////////////////////////////////////////////////
DBInterface::~DBInterface()
{
   db_.close();
}

////////////////////////////////////////////////////////////////////////////////
void DBInterface::reset(LMDBEnv* envPtr)
{
   if (db_.isOpen())
      db_.close();

   dbEnv_ = envPtr;
   auto tx = LMDBEnv::Transaction(dbEnv_, LMDB::ReadWrite);
   db_.open(dbEnv_, dbName_);
}

////////////////////////////////////////////////////////////////////////////////
void DBInterface::loadAllEntries(const SecureBinaryData& rootKey)
{
   //to keep track of dbkey gaps
   set<unsigned> gaps;
   SecureBinaryData decrPrivKey;
   SecureBinaryData macKey;

   auto&& saltedRoot = BtcUtils::getHMAC256(controlSalt_, rootKey);

   //key derivation method
   auto computeKeyPair = [&saltedRoot, &decrPrivKey, &macKey](unsigned hmacKeyInt)
   {
      SecureBinaryData hmacKey((uint8_t*)&hmacKeyInt, 4);
      auto hmacVal = BtcUtils::getHMAC512(hmacKey, saltedRoot);

      //first half is the encryption key, second half is the hmac key
      BinaryRefReader brr(hmacVal.getRef());
      decrPrivKey = move(brr.get_SecureBinaryData(32));
      macKey = move(brr.get_SecureBinaryData(32));

      //decryption private key sanity check
      if (!CryptoECDSA::checkPrivKeyIsValid(decrPrivKey))
         throw WalletInterfaceException("invalid decryptin private key");
   };

   //init first decryption key pair
   unsigned decrKeyCounter = 0;
   computeKeyPair(decrKeyCounter);

   //meta data handling lbd
   auto processMetaDataPacket = [&gaps, computeKeyPair, &decrKeyCounter]
   (const BinaryData& packet)->bool
   {
      if (packet.getSize() > erasurePlaceHolder_.getSize())
      {
         BinaryRefReader brr(packet.getRef());
         auto placeHolder = 
            brr.get_BinaryDataRef(erasurePlaceHolder_.getSize());

         if (placeHolder == erasurePlaceHolder_)
         {
            auto len = brr.get_var_int();
            if (len == 4)
            {
               auto key = brr.get_BinaryData(4);
               auto gapInt = READ_UINT32_BE(key);

               auto gapIter = gaps.find(gapInt);
               if (gapIter == gaps.end())
               {
                  throw WalletInterfaceException(
                     "erasure place holder for missing gap");
               }

               gaps.erase(gapIter);
               return true;
            }
         }
      }

      if (packet == keyCycleFlag_)
      {
         //cycle key
         ++decrKeyCounter;
         computeKeyPair(decrKeyCounter);
         return true;
      }

      return false;
   };

   /*****/

   {
      //read all db entries
      auto tx = LMDBEnv::Transaction(dbEnv_, LMDB::ReadOnly);

      int prevDbKey = -1;
      auto iter = db_.begin();
      while (iter.isValid())
      {
         auto key_mval = iter.key();
         if (key_mval.mv_size != 4)
            throw WalletInterfaceException("invalid dbkey");

         auto val_mval = iter.value();

         BinaryDataRef key_bdr((const uint8_t*)key_mval.mv_data, key_mval.mv_size);
         BinaryDataRef val_bdr((const uint8_t*)val_mval.mv_data, val_mval.mv_size);

         //dbkeys should be consecutive integers, mark gaps
         int dbKeyInt = READ_UINT32_BE(key_bdr);
         if (dbKeyInt - prevDbKey != 1)
         {
            for (unsigned i = prevDbKey + 1; i < dbKeyInt; i++)
               gaps.insert(i);
         }

         //set lowest seen integer key
         prevDbKey = dbKeyInt;

         //grab the data
         auto dataPair = readDataPacket(
            key_bdr, val_bdr, decrPrivKey, macKey);

         /*
         Check if packet is meta data.
         Meta data entries have an empty data key.
         */
         if (dataPair.first.getSize() == 0)
         {
            if (!processMetaDataPacket(dataPair.second))
               throw WalletInterfaceException("empty data key");

            iter.advance();
            continue;
         }

         auto&& keyPair = make_pair(dataPair.first, move(key_bdr.copy()));
         auto insertIter = dataKeyToDbKey_.emplace(keyPair);
         if (!insertIter.second)
            throw WalletInterfaceException("duplicated db entry");

         dataMap_.emplace(dataPair);
         iter.advance();
      }

      //sanity check
      if (gaps.size() != 0)
         throw WalletInterfaceException("unfilled dbkey gaps!");

      //set dbkey counter
      dbKeyCounter_.store(prevDbKey + 1, memory_order_relaxed);
   }

   {
      /*
      Append a key cycling flag to the this DB. All data written during
      this session will use the next key in line. This flag will signify
      the next wallet load to cycle the key accordingly to decrypt this
      new data correctly.
      */
      auto tx = LMDBEnv::Transaction(dbEnv_, LMDB::ReadWrite);

      auto flagKey = getNewDbKey();
      auto&& encrPubKey = CryptoECDSA().ComputePublicKey(decrPrivKey, true);;
      auto flagPacket = createDataPacket(
         flagKey, BinaryData(), keyCycleFlag_, encrPubKey, macKey);

      CharacterArrayRef carKey(flagKey.getSize(), flagKey.getPtr());
      CharacterArrayRef carVal(flagPacket.getSize(), flagPacket.getPtr());

      db_.insert(carKey, carVal);
   }

   //cycle to next key for this session
   ++decrKeyCounter;
   computeKeyPair(decrKeyCounter);

   //set mac key for the current session
   encrPubKey_ = CryptoECDSA().ComputePublicKey(decrPrivKey, true);
   macKey_ = move(macKey);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryDataRef DBInterface::getDataRef(const BinaryData& key) const
{
   auto iter = dataMap_.find(key);
   if (iter == dataMap_.end())
      return BinaryDataRef();

   return iter->second.getRef();
}

////////////////////////////////////////////////////////////////////////////////
void DBInterface::update(const std::vector<std::shared_ptr<InsertData>>& vec)
{
   for (auto& dataPtr : vec)
   {
      if (!dataPtr->write_)
      {
         dataMap_.erase(dataPtr->key_);
         continue;
      }

      auto insertIter = dataMap_.insert(make_pair(dataPtr->key_, dataPtr->value_));
      if (!insertIter.second)
         insertIter.first->second = dataPtr->value_;
   }
}

////////////////////////////////////////////////////////////////////////////////
void DBInterface::wipe(const BinaryData& key)
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   db_.wipe(carKey);
}

////////////////////////////////////////////////////////////////////////////////
bool DBInterface::resolveDataKey(const BinaryData& dataKey,
   BinaryData& dbKey)
{
   /*
   Return the dbKey for the data key if it exists, otherwise increment the
   dbKeyCounter and construct a key from that.
   */

   auto iter = dataKeyToDbKey_.find(dataKey);
   if (iter != dataKeyToDbKey_.end())
   {
      dbKey = iter->second;
      return true;
   }

   dbKey = getNewDbKey();
   return false;
}

////////////////////////////////////////////////////////////////////////////////
BinaryData DBInterface::getNewDbKey()
{
   auto dbKeyInt = dbKeyCounter_.fetch_add(1, memory_order_relaxed);
   return WRITE_UINT32_BE(dbKeyInt);
}

////////////////////////////////////////////////////////////////////////////////
BinaryData DBInterface::createDataPacket(const BinaryData& dbKey,
   const BinaryData& dataKey, const BinaryData& dataVal,
   const SecureBinaryData& encrPubKey, const SecureBinaryData& macKey)
{
   /* authentitcation leg */
      //concatenate dataKey and dataVal to create payload
      BinaryWriter bw;
      bw.put_var_int(dataKey.getSize());
      bw.put_BinaryData(dataKey);
      bw.put_var_int(dataVal.getSize());
      bw.put_BinaryData(dataVal);

      //append dbKey to payload
      BinaryWriter bwHmac;
      bwHmac.put_BinaryData(bw.getData());
      bwHmac.put_BinaryData(dbKey);

      //hmac (payload | dbKey)
      auto&& hmac = BtcUtils::getHMAC256(macKey, bwHmac.getData());

      //append payload to hmac
      BinaryWriter bwData;
      bwData.put_BinaryData(hmac);
      bwData.put_BinaryData(bw.getData());

      //pad payload to modulo blocksize

   /* encryption key generation */
      //generate local encryption private key
      auto&& localPrivKey = CryptoECDSA().createNewPrivateKey();
      
      //generate compressed pubkey
      auto&& localPubKey = CryptoECDSA().ComputePublicKey(localPrivKey, true);

      //ECDH local private key with encryption public key
      auto&& ecdhPubKey = 
         CryptoECDSA::PubKeyScalarMultiply(encrPubKey, localPrivKey);

      //hash256 the key as stand in for KDF
      auto&& encrKey = BtcUtils::hash256(ecdhPubKey);

   /* encryption leg */
      //generate IV
      auto&& iv = CryptoPRNG::generateRandom(
         Cipher::getBlockSize(CipherType_AES));

      //AES_CBC (hmac | payload)
      auto&& cipherText = CryptoAES::EncryptCBC(
         bwData.getData(), encrKey, iv);

      //build IES packet
      BinaryWriter encrPacket;
      encrPacket.put_BinaryData(localPubKey); 
      encrPacket.put_BinaryData(iv);
      encrPacket.put_BinaryData(cipherText);

   return encrPacket.getData();
}

////////////////////////////////////////////////////////////////////////////////
pair<BinaryData, BinaryData> DBInterface::readDataPacket(
   const BinaryData& dbKey, const BinaryData& dataPacket,
   const SecureBinaryData& decrPrivKey, const SecureBinaryData& macKey)
{
   /* decryption key */
      //recover public key
      BinaryRefReader brrCipher(dataPacket.getRef());

      //public key
      auto&& localPubKey = brrCipher.get_SecureBinaryData(33);

      //ECDH with decryption private key
      auto&& ecdhPubKey = 
         CryptoECDSA::PubKeyScalarMultiply(localPubKey, decrPrivKey);

      //kdf
      auto&& decrKey = BtcUtils::getHash256(ecdhPubKey);

   /* decryption leg */
      //get iv
      auto&& iv = brrCipher.get_SecureBinaryData(
         Cipher::getBlockSize(CipherType_AES));

      //get cipher text
      auto&& cipherText = brrCipher.get_SecureBinaryData(
         brrCipher.getSizeRemaining());
      
      //decrypt
      auto&& plainText = CryptoAES::DecryptCBC(cipherText, decrKey, iv);

   /* authentication leg */
      BinaryRefReader brrPlain(plainText.getRef());
      
      //grab hmac
      auto hmac = brrPlain.get_BinaryData(32);

      //grab data key
      auto len = brrPlain.get_var_int();
      auto dataKey = brrPlain.get_BinaryData(len);

      //grab data val
      len = brrPlain.get_var_int();
      auto dataVal = brrPlain.get_BinaryData(len);

      //mark the position
      auto pos = brrPlain.getPosition() - 32;

      //sanity check
      if (brrPlain.getSizeRemaining() != 0)
         throw WalletInterfaceException("loose data entry");

      //reset reader & grab data packet
      brrPlain.resetPosition();
      brrPlain.advance(32);
      auto data = brrPlain.get_BinaryData(pos);

      //append db key
      data.append(dbKey);

      //compute hmac
      auto computedHmac = BtcUtils::getHMAC256(macKey, data);

      //check hmac
      if (computedHmac != hmac)
         throw WalletInterfaceException("mac mismatch");

   return make_pair(dataKey, dataVal);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// WalletDBInterface
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::setupEnv(const string& path,
   const PassphraseLambda& passLbd)
{
   //sanity check
   if (!passLbd)
      throw WalletInterfaceException("null passphrase lambda");

   auto lock = unique_lock<mutex>(setupMutex_);
   if (dbEnv_ != nullptr)
      return;

   path_ = path;

   //open env for control and meta dbs
   dbEnv_ = make_unique<LMDBEnv>(2);
   dbEnv_->open(path, MDB_WRITEMAP);
   // Increase wallet size from 1 mb to 50 mb to prevent signer crashes with
   // with "MDB_MAP_FULL: Environment mapsize limit reached" error
   // when there are a lot of used addresses
   dbEnv_->setMapSize(50*1024*1024);

   //open control db
   openControlDb();

   bool isNew = false;
   shared_ptr<WalletHeader_Control> controlHeader;
   try
   {
      //get control header
      controlHeader = dynamic_pointer_cast<WalletHeader_Control>(
         loadControlHeader());
      if (controlHeader == nullptr)
         throw WalletException("invalid control header");
   }
   catch (NoEntryInWalletException&)
   {
      //no control header, this is a fresh wallet, set it up
      controlHeader = setupControlDB(passLbd);
      isNew = true;
   }

   //load control decrypted data container
   loadDataContainer(controlHeader);

   //load control seed
   loadSeed(controlHeader);

   /*
   The passphrase prompt will be called a 3rd time out of 3 in this 
   scope to decrypt the control seed and generate the encrypted 
   header DB.
   */

   //decrypt control seed
   lockControlContainer(passLbd);
   auto& rootEncrKey = 
      decryptedData_->getDecryptedPrivateData(controlSeed_.get());

   //load wallet header db
   {
      auto headrPtr = make_shared<WalletHeader_Control>();
      headrPtr->walletID_ = BinaryData(WALLETHEADER_DBNAME);
      headrPtr->controlSalt_ = controlHeader->controlSalt_;
      openDB(headrPtr, rootEncrKey);
   }

   //load wallet header objects
   unsigned dbCount;
   if (!isNew)
   {
      loadHeaders();
      dbCount = headerMap_.size() + 2;
   }
   else
   {
      dbCount = 3;
   }

   //set new db count;
   setDbCount(dbCount, false);

   //open all dbs listed in header map
   for (auto& headerPtr : headerMap_)
      openDB(headerPtr.second, rootEncrKey);

   //clean up
   unlockControlContainer();
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef WalletDBInterface::getDataRefForKey(
   DBIfaceTransaction* tx, const BinaryData& key)
{
   /** The reference lifetime is tied to the db tx lifetime. The caller has to
   maintain the tx for as long as the data ref needs to be valid **/

   auto ref = tx->getDataRef(key);

   if (ref.getSize() == 0)
      throw NoEntryInWalletException();

   return DBUtils::getDataRefForPacket(ref);
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::loadHeaders()
{
   auto&& tx = beginReadTransaction(WALLETHEADER_DBNAME);

   //meta map
   auto dbIter = tx->getIterator();

   BinaryWriter bwKey;
   bwKey.put_uint8_t(WALLETHEADER_PREFIX);
   dbIter->seek(bwKey.getDataRef());

   while (dbIter->isValid())
   {
      auto iterkey = dbIter->key();
      auto itervalue = dbIter->value();

      //check value's advertized size is packet size and strip it
      BinaryRefReader brrVal(itervalue);
      auto valsize = brrVal.get_var_int();
      if (valsize != brrVal.getSizeRemaining())
         throw WalletInterfaceException("entry val size mismatch");

      try
      {
         auto headerPtr = WalletHeader::deserialize(
            iterkey, brrVal.get_BinaryDataRef(brrVal.getSizeRemaining()));
         //headerPtr->masterID_ = masterID_;

         if (headerPtr->shouldLoad())
            headerMap_.insert(make_pair(headerPtr->getWalletID(), headerPtr));
      }
      catch (exception& e)
      {
         LOGERR << e.what();
         break;
      }

      dbIter->advance();
   }
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::openControlDb(void)
{
   if (controlDb_ != nullptr)
      throw WalletInterfaceException("controlDb is not null");

   controlDb_ = make_unique<LMDB>();
   auto tx = LMDBEnv::Transaction(dbEnv_.get(), LMDB::ReadWrite);
   controlDb_->open(dbEnv_.get(), CONTROL_DB_NAME);
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::shutdown()
{
   auto lock = unique_lock<mutex>(setupMutex_);
   if (DBIfaceTransaction::hasTx())
      throw WalletInterfaceException("live transactions, cannot shutdown env");

   if (controlDb_ != nullptr)
   {
      controlDb_->close();
      controlDb_.reset();
   }

   controlLock_.reset();
   decryptedData_.reset();
   controlSeed_.reset();

   dbMap_.clear();

   if (dbEnv_ != nullptr)
   {
      dbEnv_->close();
      dbEnv_.reset();
   }

   dbCount_ = 0;
   path_.clear();
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::openDB(std::shared_ptr<WalletHeader> headerPtr,
   const SecureBinaryData& encrRootKey)
{
   auto&& dbName = headerPtr->getDbName();
   auto iter = dbMap_.find(dbName);
   if (iter != dbMap_.end())
      return;

   //create db object
   auto dbiPtr = make_unique<DBInterface>(
      dbEnv_.get(), dbName, headerPtr->controlSalt_);
   
   /*
   Load all db entries in RAM. This call also decrypts the on disk data.
   */
   dbiPtr->loadAllEntries(encrRootKey);

   //insert in dbMap
   dbMap_.insert(make_pair(dbName, move(dbiPtr)));

}

////////////////////////////////////////////////////////////////////////////////
const string& WalletDBInterface::getFilename() const
{
   if (dbEnv_ == nullptr)
      throw WalletInterfaceException("null dbEnv");

   return dbEnv_->getFilename();
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<DBIfaceTransaction> WalletDBInterface::beginWriteTransaction(
   const string& dbName)
{
   auto iter = dbMap_.find(dbName);
   if (iter == dbMap_.end())
   {
      if (dbName == CONTROL_DB_NAME)
      {
         return make_unique<RawIfaceTransaction>(
            dbEnv_.get(), controlDb_.get(), true);
      }

      throw WalletInterfaceException("invalid db name");
   }

   return make_unique<WalletIfaceTransaction>(iter->second.get(), true);
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<DBIfaceTransaction> WalletDBInterface::beginReadTransaction(
   const string& dbName)
{
   auto iter = dbMap_.find(dbName);
   if (iter == dbMap_.end())
   {
      if (dbName == CONTROL_DB_NAME)
      {
         return make_unique<RawIfaceTransaction>(
            dbEnv_.get(), controlDb_.get(), false);
      }

      throw WalletInterfaceException("invalid db name");
   }

   return make_unique<WalletIfaceTransaction>(iter->second.get(), false);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<WalletHeader> WalletDBInterface::loadControlHeader()
{
   //grab meta object
   BinaryWriter bw;
   bw.put_uint8_t(WALLETHEADER_PREFIX);
   bw.put_BinaryData(BinaryData(CONTROL_DB_NAME));
   auto& headerKey = bw.getData();

   auto&& tx = beginReadTransaction(CONTROL_DB_NAME);
   auto headerVal = getDataRefForKey(tx.get(), headerKey);
   if (headerVal.getSize() == 0)
      throw WalletInterfaceException("missing control db entry");

   return WalletHeader::deserialize(headerKey, headerVal);
}

////////////////////////////////////////////////////////////////////////////////
void MockDeleteWalletDBInterface(WalletDBInterface* wdbi)
{
   /*
   To create the DecryptedDataContainer for the control header, we need to pass 
   it a shared_ptr of -this-. This dcc is tied to the setupEnv scope, and we do
   not want it to delete -this- when it is destroyed and takes the wdbi 
   shared_ptr with it. Therefor we create a shared_ptr from -this-, passing it
   a deleter that does in effect nothing.
   */
   return;
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::loadDataContainer(shared_ptr<WalletHeader> headerPtr)
{
   //grab decrypted data object
   shared_ptr<WalletDBInterface> ifacePtr(this, MockDeleteWalletDBInterface);
   decryptedData_ = make_unique<DecryptedDataContainer>(
      ifacePtr, headerPtr->getDbName(),
      headerPtr->getDefaultEncryptionKey(),
      headerPtr->getDefaultEncryptionKeyId(),
      headerPtr->defaultKdfId_, headerPtr->masterEncryptionKeyId_);
   decryptedData_->readFromDisk();
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::loadSeed(shared_ptr<WalletHeader> headerPtr)
{
   auto&& tx = beginReadTransaction(headerPtr->getDbName());

   BinaryWriter bwKey;
   bwKey.put_uint32_t(WALLET_SEED_KEY);
   auto rootAssetRef = getDataRefForKey(tx.get(), bwKey.getData());

   auto seedPtr = Asset_EncryptedData::deserialize(
      rootAssetRef.getSize(), rootAssetRef);
   auto ptrCast = dynamic_cast<EncryptedSeed*>(seedPtr.get());
   if (ptrCast == nullptr)
      throw WalletException("failed to deser wallet seed");

   controlSeed_ = unique_ptr<EncryptedSeed>(ptrCast);
   seedPtr.release();
}

////////////////////////////////////////////////////////////////////////////////
MasterKeyStruct WalletDBInterface::initWalletHeaderObject(
   shared_ptr<WalletHeader> headerPtr, const SecureBinaryData& passphrase)
{
   /*
   Setup master and top encryption key.

   - The master encryption key encrypts entries in the wallet.

   - The top encryption key encrypts the master encryption key.
     If a user passphrase is provided, it is used to generate the top encryption
     key. Otherwise the default encryption key is used.

   - The default encryption key is 32 byte RNG value written in clear text on
     disk. Its purpose is to prevent divergence in implemenation between
     encrypted and unencrypted wallets.
   */

   MasterKeyStruct mks;

   /*
   generate master encryption key, derive id
   */
   mks.kdf_ = make_shared<KeyDerivationFunction_Romix>();
   auto&& masterKeySBD = CryptoPRNG::generateRandom(32);
   mks.decryptedMasterKey_ = make_shared<DecryptedEncryptionKey>(masterKeySBD);
   mks.decryptedMasterKey_->deriveKey(mks.kdf_);
   auto&& masterEncryptionKeyId = mks.decryptedMasterKey_->getId(mks.kdf_->getId());

   /*
   create cipher, tie it to master encryption key
   */
   mks.cipher_ = make_unique<Cipher_AES>(mks.kdf_->getId(),
      masterEncryptionKeyId);

   /*
   setup default encryption key, only ever used if no user passphrase is
   provided
   */
   headerPtr->defaultEncryptionKey_ = move(CryptoPRNG::generateRandom(32));
   auto defaultKey = headerPtr->getDefaultEncryptionKey();
   auto defaultEncryptionKeyPtr = make_unique<DecryptedEncryptionKey>(defaultKey);
   defaultEncryptionKeyPtr->deriveKey(mks.kdf_);
   headerPtr->defaultEncryptionKeyId_ =
      defaultEncryptionKeyPtr->getId(mks.kdf_->getId());

   /*
   encrypt master encryption key with passphrase if present, otherwise use
   default key
   */
   unique_ptr<DecryptedEncryptionKey> topEncryptionKey;
   if (passphrase.getSize() > 0)
   {
      //copy passphrase
      auto&& passphraseCopy = passphrase.copy();
      topEncryptionKey = make_unique<DecryptedEncryptionKey>(passphraseCopy);
   }
   else
   {
      LOGWARN << "Wallet created without password, using default encryption key";
      topEncryptionKey = move(defaultEncryptionKeyPtr);
   }

   /*
   derive encryption key id
   */
   topEncryptionKey->deriveKey(mks.kdf_);
   auto&& topEncryptionKeyId = topEncryptionKey->getId(mks.kdf_->getId());

   /*
   create cipher for top encryption key
   */
   auto&& masterKeyCipher = mks.cipher_->getCopy(topEncryptionKeyId);

   /*
   encrypt the master encryption key with the top encryption key
   */
   auto&& encrMasterKey = masterKeyCipher->encrypt(
      topEncryptionKey.get(), mks.kdf_->getId(), 
      mks.decryptedMasterKey_.get());

   /*
   create encryption key object
   */
   mks.masterKey_ = make_shared<Asset_EncryptionKey>(masterEncryptionKeyId,
      encrMasterKey, move(masterKeyCipher));

   /*
   set master encryption key relevant ids in the WalletMeta object
   */
   headerPtr->masterEncryptionKeyId_ = mks.masterKey_->getId();
   headerPtr->defaultKdfId_ = mks.kdf_->getId();

   /*
   setup control salt
   */
   headerPtr->controlSalt_ = CryptoPRNG::generateRandom(32);

   return move(mks);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<WalletHeader_Control> WalletDBInterface::setupControlDB(
   const PassphraseLambda& passLbd)
{
   //prompt for passphrase
   SecureBinaryData passphrase = passLbd(set<BinaryData>());

   //create control meta object
   auto headerPtr = make_shared<WalletHeader_Control>();
   headerPtr->walletID_ = BinaryData(CONTROL_DB_NAME);
   auto keyStruct = initWalletHeaderObject(headerPtr, passphrase);

   //setup controlDB decrypted data container
   shared_ptr<WalletDBInterface> ifacePtr(this, MockDeleteWalletDBInterface);
   auto decryptedData = make_shared<DecryptedDataContainer>(
      ifacePtr, CONTROL_DB_NAME,
      headerPtr->defaultEncryptionKey_,
      headerPtr->defaultEncryptionKeyId_,
      headerPtr->defaultKdfId_,
      headerPtr->masterEncryptionKeyId_);
   decryptedData->addEncryptionKey(keyStruct.masterKey_);
   decryptedData->addKdf(keyStruct.kdf_);

   /*
   The lambda will be called to trigger the encryption of the control seed.
   This will be the second out of 3 calls to the passphrase lambda during
   wallet creation.
   */
   decryptedData->setPassphrasePromptLambda(passLbd);

   {
      //create encrypted seed object
      auto&& seed = CryptoPRNG::generateRandom(32);
      auto&& lock = ReentrantLock(decryptedData.get());

      auto cipherCopy = keyStruct.cipher_->getCopy();
      auto&& cipherText = decryptedData->encryptData(
         cipherCopy.get(), seed);
      auto encrSeed = make_shared<EncryptedSeed>(cipherText, move(cipherCopy));

      //write seed to disk
      auto&& tx = beginWriteTransaction(CONTROL_DB_NAME);

      BinaryWriter seedKey;
      seedKey.put_uint32_t(WALLET_SEED_KEY);
      auto&& seedVal = encrSeed->serialize();
      tx->insert(seedKey.getData(), seedVal);

      //write meta ptr to disk
      auto&& metaKey = headerPtr->getDbKey();
      auto&& metaVal = headerPtr->serialize();
      tx->insert(metaKey, metaVal);

      //write decrypted data container to disk
      decryptedData->updateOnDisk();
   }

   return headerPtr;
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::putHeader(shared_ptr<WalletHeader> headerPtr)
{
   auto&& key = headerPtr->getDbKey();
   auto&& val = headerPtr->serialize();

   auto&& tx = beginWriteTransaction(WALLETHEADER_DBNAME);
   tx->insert(key, val);
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::addHeader(std::shared_ptr<WalletHeader> headerPtr)
{
   auto lock = unique_lock<mutex>(setupMutex_);
   
   auto iter = headerMap_.find(headerPtr->walletID_);
   if (iter != headerMap_.end())
      throw WalletInterfaceException("header already in map");
   
   if (dbMap_.size() + 2 > dbCount_)
      throw WalletInterfaceException("dbCount is too low");
 
   auto&& dbName = headerPtr->getDbName();
   if (dbName.size() == 0)
      throw WalletInterfaceException("empty dbname");

   auto& rootEncrKey = 
      decryptedData_->getDecryptedPrivateData(controlSeed_.get());
   auto dbiPtr = make_unique<DBInterface>(
      dbEnv_.get(), dbName, headerPtr->controlSalt_);
   dbiPtr->loadAllEntries(rootEncrKey);
   
   putHeader(headerPtr);
   dbMap_.insert(make_pair(dbName, move(dbiPtr)));
   headerMap_.insert(make_pair(headerPtr->walletID_, headerPtr));
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<WalletHeader> WalletDBInterface::getWalletHeader(
   const string& name) const
{
   auto iter = headerMap_.find(name);
   if (iter == headerMap_.end())
      throw WalletException("missing main wallet header");
   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
const map<BinaryData, shared_ptr<WalletHeader>>& 
WalletDBInterface::getHeaderMap() const
{
   return headerMap_;
}

////////////////////////////////////////////////////////////////////////////////
unsigned WalletDBInterface::getDbCount() const
{
   auto lock = unique_lock<mutex>(setupMutex_);
   return headerMap_.size();
}

////////////////////////////////////////////////////////////////////////////////
unsigned WalletDBInterface::getFreeDbCount() const
{
   auto lock = unique_lock<mutex>(setupMutex_);
   auto count = headerMap_.size() + 2;
   if (count >= dbCount_)
      return 0;

   return dbCount_ - count;
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::setDbCount(unsigned count)
{
   //add 2 for the control and headers db
   setDbCount(count + 2, true);
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::setDbCount(unsigned count, bool doLock)
{
   if (DBIfaceTransaction::hasTx())
   {
      throw WalletInterfaceException(
         "live transactions, cannot change dbCount");
   }

   if (count <= dbCount_)
      return;

   auto lock = unique_lock<mutex>(setupMutex_, defer_lock);
   if (doLock)
      lock.lock();

   //close env
   if (controlDb_ != nullptr)
   {
      controlDb_->close();
      controlDb_.reset();
   }

   for (auto& dbPtr : dbMap_)
      dbPtr.second->close();

   dbEnv_->close();
   dbEnv_.reset();

   //reopen with new dbCount
   dbEnv_ = make_unique<LMDBEnv>(count);
   dbEnv_->open(path_, MDB_WRITEMAP);

   for (auto& dbPtr : dbMap_)
   {
      auto&& tx = beginWriteTransaction(dbPtr.first);
      dbPtr.second->reset(dbEnv_.get());
   }

   dbCount_ = count;
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::lockControlContainer(const PassphraseLambda& passLbd)
{
   if (controlLock_ != nullptr)
      throw WalletInterfaceException("control container already locked");
   
   controlLock_ = make_unique<ReentrantLock>(decryptedData_.get());
   decryptedData_->setPassphrasePromptLambda(passLbd);
}

////////////////////////////////////////////////////////////////////////////////
void WalletDBInterface::unlockControlContainer()
{
   if (controlLock_ == nullptr)
      throw WalletInterfaceException("control container isn't locked");

   decryptedData_->resetPassphraseLambda();
   controlLock_.reset();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// DBIfaceIterator
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
DBIfaceIterator::~DBIfaceIterator()
{}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// WalletIfaceIterator
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
bool WalletIfaceIterator::isValid() const
{
   return iterator_ != dbPtr_->dataMap_.end();
}

////////////////////////////////////////////////////////////////////////////////
void WalletIfaceIterator::seek(const BinaryDataRef& key)
{
   iterator_ = dbPtr_->dataMap_.lower_bound(key);
}

////////////////////////////////////////////////////////////////////////////////
void WalletIfaceIterator::advance()
{
   ++iterator_;
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef WalletIfaceIterator::key() const
{
   return iterator_->first.getRef();
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef WalletIfaceIterator::value() const
{
   return iterator_->second.getRef();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// RawIfaceIterator
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
bool RawIfaceIterator::isValid() const
{
   return iterator_.isValid();
}

////////////////////////////////////////////////////////////////////////////////
void RawIfaceIterator::seek(const BinaryDataRef& key)
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   iterator_.seek(carKey, LMDB::Iterator::SeekBy::Seek_GE);
}

////////////////////////////////////////////////////////////////////////////////
void RawIfaceIterator::advance()
{
   ++iterator_;
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef RawIfaceIterator::key() const
{
   auto val = iterator_.key();
   return BinaryDataRef((const uint8_t*)val.mv_data, val.mv_size);
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef RawIfaceIterator::value() const
{
   auto val = iterator_.value();
   return BinaryDataRef((const uint8_t*)val.mv_data, val.mv_size);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// DBIfaceTransaction
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
map<string, map<thread::id, DBIfaceTransaction::ParentTx>> 
DBIfaceTransaction::txMap_;

mutex DBIfaceTransaction::txMutex_;

////////////////////////////////////////////////////////////////////////////////
DBIfaceTransaction::~DBIfaceTransaction() noexcept(false)
{}

////////////////////////////////////////////////////////////////////////////////
bool DBIfaceTransaction::hasTx()
{
   auto lock = unique_lock<mutex>(txMutex_);
   for (auto& txPair : txMap_)
   {
      if (txPair.second.size() > 0)
         return true;
   }

   return false;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// WalletIfaceTransaction
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
WalletIfaceTransaction::WalletIfaceTransaction(
   DBInterface* dbPtr, bool mode) :
   DBIfaceTransaction(), dbPtr_(dbPtr), commit_(mode)
{
   if (!insertTx(this))
      throw WalletInterfaceException("failed to create db tx");
}

////////////////////////////////////////////////////////////////////////////////
WalletIfaceTransaction::~WalletIfaceTransaction() noexcept(false)
{
   LMDBEnv::Transaction tx;

   {
      auto lock = unique_lock<mutex>(txMutex_);
      if (!eraseTx(this))
         return;

      if (!commit_)
         return;

      tx = move(LMDBEnv::Transaction(dbPtr_->dbEnv_, LMDB::ReadWrite));
   }

   
   //this is the top tx, need to commit all this data to the db object
   for (unsigned i=0; i < insertVec_.size(); i++)
   {
      auto dataPtr = insertVec_[i];

      //is this operation is the last for this data key?
      auto effectIter = keyToDataMap_.find(dataPtr->key_);
      if (effectIter == keyToDataMap_.end())
      {   
         throw WalletInterfaceException(
            "insert operation is not mapped to data key!");
      }

      //skip if this isn't the last effect
      if (i != effectIter->second)
         continue;

      BinaryData dbKey;
      auto keyExists = dbPtr_->resolveDataKey(dataPtr->key_, dbKey);
      if (keyExists)
      {
         /***
            This operation abuses the no copy read feature in lmdb. Since all data is
            mmap'd, a no copy read is a pointer to the data on disk. Therefor modifying
            that data will result in a modification on disk.

            This is done under 3 conditions:
            1) The decrypted data container is locked.
            2) The calling threads owns a ReadWrite transaction on the lmdb object
            3) There are no active ReadOnly transactions on the lmdb object

            1. is a no brainer, 2. guarantees the changes are flushed to disk once the
            tx is released. RW tx are locked, therefor only one is active at any given
            time, by LMDB design.

            3. is to guarantee there are no readers when the change takes place. Needs
            some LMDB C++ wrapper modifications to be able to check from the db object.
            The condition should be enforced by the caller regardless.
         ***/

         //wipe the key
         CharacterArrayRef carKey(dbKey.getSize(), dbKey.getPtr());
         dbPtr_->db_.wipe(carKey);

         //create erasure place holder packet
         BinaryWriter erasedBw;
         erasedBw.put_String("erased");
         erasedBw.put_var_int(dbKey.getSize());
         erasedBw.put_BinaryData(dbKey);

         //get new key
         dbKey = dbPtr_->getNewDbKey();

         //commit erasure packet
         auto&& dbVal = DBInterface::createDataPacket(
            dbKey, BinaryData(), erasedBw.getData(), 
            dbPtr_->encrPubKey_, dbPtr_->macKey_);

         CharacterArrayRef carData(dbVal.getSize(), dbVal.getPtr());
         CharacterArrayRef carKey2(dbKey.getSize(), dbKey.getPtr());
         dbPtr_->db_.insert(carKey2, carData);

         //move on to next piece of data if there is nothing to write
         if (!dataPtr->write_)
         {
            //update dataKeyToDbKey
            dbPtr_->dataKeyToDbKey_.erase(dataPtr->key_);
            continue;
         }

         //grab a fresh key for the follow up write
         dbKey = dbPtr_->getNewDbKey();
      }

      //sanity check
      if (!dataPtr->write_)
         throw WalletInterfaceException("key marked for deletion when it does not exist");

      //update dataKeyToDbKey
      dbPtr_->dataKeyToDbKey_[dataPtr->key_] = dbKey;

      //bundle key and val together, key by dbkey
      auto&& dbVal = DBInterface::createDataPacket(
         dbKey, dataPtr->key_, dataPtr->value_, 
         dbPtr_->encrPubKey_, dbPtr_->macKey_);
      CharacterArrayRef carKey(dbKey.getSize(), dbKey.getPtr());
      CharacterArrayRef carVal(dbVal.getSize(), dbVal.getPtr());

      dbPtr_->db_.insert(carKey, carVal);
   }

   //update db data map
   dbPtr_->update(insertVec_);
}

////////////////////////////////////////////////////////////////////////////////
bool WalletIfaceTransaction::insertTx(WalletIfaceTransaction* txPtr)
{
   if (txPtr == nullptr)
      throw WalletInterfaceException("null tx ptr");

   auto lock = unique_lock<mutex>(txMutex_);

   auto dbIter = txMap_.find(txPtr->dbPtr_->getName());
   if (dbIter == txMap_.end())
   {
      dbIter = txMap_.insert(
         make_pair(txPtr->dbPtr_->getName(), map<thread::id, ParentTx>())
      ).first;
   }

   auto& txMap = dbIter->second;

   //save tx by thread id
   auto thrId = this_thread::get_id();
   auto iter = txMap.find(thrId);
   if (iter == txMap.end())
   {
      //this is the parent tx, create the lambdas and setup the struct
      
      ParentTx ptx;
      ptx.commit_ = txPtr->commit_;

      if (txPtr->commit_)
      {
         auto insertLbd = [thrId, txPtr](const BinaryData& key, const BinaryData& val)
         {
            if (thrId != this_thread::get_id())
               throw WalletInterfaceException("insert operation thread id mismatch");

            auto dataPtr = make_shared<InsertData>();
            dataPtr->key_ = key;
            dataPtr->value_ = val;

            unsigned vecSize = txPtr->insertVec_.size();
            txPtr->insertVec_.emplace_back(dataPtr);

            /*
            Insert the index for this data object in the key map.
            Replace the index if it's already there as we want to track
            the final effect for each key.
            */
            auto insertPair = txPtr->keyToDataMap_.insert(make_pair(key, vecSize));
            if (!insertPair.second)
               insertPair.first->second = vecSize;
         };

         auto eraseLbd = [thrId, txPtr](const BinaryData& key, bool wipe)
         {
            if (thrId != this_thread::get_id())
               throw WalletInterfaceException("insert operation thread id mismatch");

            auto dataPtr = make_shared<InsertData>();
            dataPtr->key_ = key;
            dataPtr->write_ = false; //set to false to signal deletion
            dataPtr->wipe_ = wipe;

            unsigned vecSize = txPtr->insertVec_.size();
            txPtr->insertVec_.emplace_back(dataPtr);

            auto insertPair = txPtr->keyToDataMap_.insert(make_pair(key, vecSize));
            if (!insertPair.second)
               insertPair.first->second = vecSize;
         };

         auto getDataLbd = [thrId, txPtr](const BinaryData& key)->
            const shared_ptr<InsertData>&
         {
            auto iter = txPtr->keyToDataMap_.find(key);
            if (iter == txPtr->keyToDataMap_.end())
               throw NoDataInDB();

            return txPtr->insertVec_[iter->second];
         };

         txPtr->insertLbd_ = insertLbd;
         txPtr->eraseLbd_ = eraseLbd;
         txPtr->getDataLbd_ = getDataLbd;

         ptx.insertLbd_ = insertLbd;
         ptx.eraseLbd_ = eraseLbd;
         ptx.getDataLbd_ = getDataLbd;
      }

      txMap.insert(make_pair(thrId, ptx));
      return true;
   }
   
   /*we already have a tx for this thread, we will nest the new one within it*/
   
   //make sure the commit type between parent and nested tx match
   if (iter->second.commit_ != txPtr->commit_)
      return false;

   //set lambdas
   txPtr->insertLbd_ = iter->second.insertLbd_;
   txPtr->eraseLbd_ = iter->second.eraseLbd_;
   txPtr->getDataLbd_ = iter->second.getDataLbd_;

   //increment counter
   ++iter->second.counter_;
   return true;
}

////////////////////////////////////////////////////////////////////////////////
bool WalletIfaceTransaction::eraseTx(WalletIfaceTransaction* txPtr)
{
   if (txPtr == nullptr)
      throw WalletInterfaceException("null tx ptr");
      
   //we have to have this db name in the tx map
   auto dbIter = txMap_.find(txPtr->dbPtr_->getName());
   if (dbIter == txMap_.end())
      throw WalletInterfaceException("missing db name in tx map");

   auto& txMap = dbIter->second;

   //thread id has to be present too
   auto thrId = this_thread::get_id();
   auto iter = txMap.find(thrId);
   if (iter == txMap.end())
      throw WalletInterfaceException("missing thread id in tx map");

   if (iter->second.counter_ > 1)
   {
      //this is a nested tx, decrement and return false
      --iter->second.counter_;
      return false;
   }

   //counter is 1, this is the parent tx, clean up the entry and return true
   txMap.erase(iter);
   return true;
}

////////////////////////////////////////////////////////////////////////////////
void WalletIfaceTransaction::insert(const BinaryData& key, const BinaryData& val)
{
   if (!insertLbd_)
      throw WalletInterfaceException("insert lambda is not set");

   insertLbd_(key, val);
}

////////////////////////////////////////////////////////////////////////////////
void WalletIfaceTransaction::erase(const BinaryData& key)
{
   if (!eraseLbd_)
      throw WalletInterfaceException("erase lambda is not set");

   eraseLbd_(key, false);
}

////////////////////////////////////////////////////////////////////////////////
void WalletIfaceTransaction::wipe(const BinaryData& key)
{
   if (!eraseLbd_)
      throw WalletInterfaceException("erase lambda is not set");

   eraseLbd_(key, true);
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<DBIfaceIterator> WalletIfaceTransaction::getIterator() const
{
   if (commit_)
      throw WalletInterfaceException("cannot iterate over a write transaction");

   return make_shared<WalletIfaceIterator>(dbPtr_);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryDataRef WalletIfaceTransaction::getDataRef(
   const BinaryData& key) const
{
   if (commit_)
   {
      /*
      A write transaction may carry data that overwrites the db object data map.
      Check the modification map first.
      */

      try
      {
         auto& dataPtr = getInsertDataForKey(key);
         if (!dataPtr->write_)
            return BinaryDataRef();

         return dataPtr->value_.getRef();
      }
      catch (NoDataInDB&)
      {
         /*
         Will throw if there's no data in the write tx.
         Look for it in the db instead.
         */
      }
   }

   return dbPtr_->getDataRef(key);
}

////////////////////////////////////////////////////////////////////////////////
const std::shared_ptr<InsertData>& WalletIfaceTransaction::getInsertDataForKey(
   const BinaryData& key) const
{
   if (!getDataLbd_)
      throw WalletInterfaceException("tx is missing get lbd");

   return getDataLbd_(key);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// RawIfaceTransaction
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void RawIfaceTransaction::insert(const BinaryData& key, const BinaryData& val)
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   CharacterArrayRef carVal(val.getSize(), val.getPtr());
   dbPtr_->insert(carKey, carVal);
}

////////////////////////////////////////////////////////////////////////////////
void RawIfaceTransaction::erase(const BinaryData& key)
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   dbPtr_->erase(carKey);
}

////////////////////////////////////////////////////////////////////////////////
void RawIfaceTransaction::wipe(const BinaryData& key)
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   dbPtr_->wipe(carKey);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryDataRef RawIfaceTransaction::getDataRef(const BinaryData& key) const
{
   CharacterArrayRef carKey(key.getSize(), key.getPtr());
   auto&& carVal = dbPtr_->get_NoCopy(carKey);

   if (carVal.len == 0)
      return BinaryDataRef();

   BinaryDataRef result((const uint8_t*)carVal.data, carVal.len);
   return result;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<DBIfaceIterator> RawIfaceTransaction::getIterator() const
{
   return make_shared<RawIfaceIterator>(dbPtr_);
}
