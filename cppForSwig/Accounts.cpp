////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2017-2019, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "DBUtils.h"
#include "Accounts.h"
#include "DecryptedDataContainer.h"
#include "BIP32_Node.h"
#include "ResolverFeed.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AssetAccount
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
size_t AssetAccount::writeAssetEntry(shared_ptr<AssetEntry> entryPtr)
{
   if (!entryPtr->needsCommit())
      return SIZE_MAX;

   auto&& tx = iface_->beginWriteTransaction(dbName_);

   auto&& serializedEntry = entryPtr->serialize();
   auto&& dbKey = entryPtr->getDbKey();

   tx->insert(dbKey, serializedEntry);

   entryPtr->doNotCommit();
   return serializedEntry.getSize();
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::updateOnDiskAssets()
{
   auto&& tx = iface_->beginWriteTransaction(dbName_);
   for (auto& entryPtr : assets_)
      writeAssetEntry(entryPtr.second);

   updateAssetCount();
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::updateAssetCount()
{
   //asset count key
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ASSET_COUNT_PREFIX);
   bwKey.put_BinaryData(getFullID());

   //asset count
   BinaryWriter bwData;
   bwData.put_var_int(assets_.size());

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->insert(bwKey.getData(), bwData.getData());
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::commit()
{
   //id as key
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ASSET_ACCOUNT_PREFIX);
   bwKey.put_BinaryData(getFullID());

   //data
   BinaryWriter bwData;

   //type
   bwData.put_uint8_t(type());

   //parent key size
   bwData.put_var_int(parent_id_.getSize());

   //der scheme
   auto&& derSchemeSerData = derScheme_->serialize();
   bwData.put_var_int(derSchemeSerData.getSize());
   bwData.put_BinaryData(derSchemeSerData);
     
   //commit root asset if there is one
   if (root_ != nullptr)
      writeAssetEntry(root_);

   //commit assets
   for (auto asset : assets_)
      writeAssetEntry(asset.second);

   //commit serialized account data
   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->insert(bwKey.getData(), bwData.getData());

   updateAssetCount();
   updateHighestUsedIndex();
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::putData(
   LMDB* db, const BinaryData& key, const BinaryData& data)
{
   CharacterArrayRef carKey(key.getSize(), key.getCharPtr());
   CharacterArrayRef carData(data.getSize(), data.getCharPtr());

   db->insert(carKey, carData);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetAccount> AssetAccount::loadFromDisk(const BinaryData& key, 
   shared_ptr<WalletDBInterface> iface, const string& dbName)
{
   auto&& tx = iface->beginReadTransaction(dbName);

   //sanity checks
   if (iface == nullptr || dbName.size() == 0)
      throw AccountException("invalid db pointers");

   if (key.getSize() == 0)
      throw AccountException("invalid key size");

   if (key.getPtr()[0] != ASSET_ACCOUNT_PREFIX)
      throw AccountException("unexpected prefix for AssetAccount key");

   auto&& diskDataRef = tx->getDataRef(key);
   BinaryRefReader brr(diskDataRef);

   //type
   auto type = AssetAccountTypeEnum(brr.get_uint8_t());

   //ids
   auto parent_id_len = brr.get_var_int();

   auto&& parent_id = key.getSliceCopy(1, parent_id_len);
   auto&& account_id = key.getSliceCopy(
      1 + parent_id_len, key.getSize() - 1 - parent_id_len);

   //der scheme
   auto len = brr.get_var_int();
   auto derSchemeBDR = DBUtils::getDataRefForPacket(brr.get_BinaryDataRef(len));
   auto derScheme = DerivationScheme::deserialize(derSchemeBDR, iface, dbName);

   //asset count
   size_t assetCount = 0;
   {
      BinaryWriter bwKey_assetcount;
      bwKey_assetcount.put_uint8_t(ASSET_COUNT_PREFIX);
      bwKey_assetcount.put_BinaryDataRef(key.getSliceRef(
         1, key.getSize() - 1));

      auto&& assetcount = tx->getDataRef(bwKey_assetcount.getData());
      if (assetcount.getSize() == 0)
         throw AccountException("missing asset count entry");

      BinaryRefReader brr_assetcount(assetcount);
      assetCount = brr_assetcount.get_var_int();
   }

   //last used index
   size_t lastUsedIndex = 0;
   {
      BinaryWriter bwKey_lastusedindex;
      bwKey_lastusedindex.put_uint8_t(ASSET_TOP_INDEX_PREFIX);
      bwKey_lastusedindex.put_BinaryDataRef(key.getSliceRef(
         1, key.getSize() - 1));

      auto&& lastusedindex = tx->getDataRef(bwKey_lastusedindex.getData());
      if (lastusedindex.getSize() == 0)
         throw AccountException("missing last used entry");

      BinaryRefReader brr_lastusedindex(lastusedindex);
      lastUsedIndex = brr_lastusedindex.get_var_int();
   }

   //asset entry prefix key
   BinaryWriter bwAssetKey;
   bwAssetKey.put_uint8_t(ASSETENTRY_PREFIX);
   bwAssetKey.put_BinaryDataRef(key.getSliceRef(1, key.getSize() - 1));
   
   //asset key
   shared_ptr<AssetEntry> rootEntry = nullptr;
   map<unsigned, shared_ptr<AssetEntry>> assetMap;
   
   //get all assets
   {
      auto& assetDbKey = bwAssetKey.getData();
      auto dbIter = tx->getIterator();
      dbIter->seek(assetDbKey);

      while (dbIter->isValid())
      {
         auto&& key_bdr = dbIter->key();
         auto&& value_bdr = dbIter->value();

         //check key isnt prefix
         if (key_bdr == assetDbKey)
            continue;

         //check key starts with prefix
         if (!key_bdr.startsWith(assetDbKey))
            break;

         //instantiate and insert asset
         auto assetPtr = AssetEntry::deserialize(
            key_bdr, 
            DBUtils::getDataRefForPacket(value_bdr));

         if (assetPtr->getIndex() != ROOT_ASSETENTRY_ID)
            assetMap.insert(make_pair(assetPtr->getIndex(), assetPtr));
         else
            rootEntry = assetPtr;

         dbIter->advance();
      } 
   }

   //sanity check
   if (assetCount != assetMap.size())
      throw AccountException("unexpected account asset count");

   //instantiate object
   shared_ptr<AssetAccount> accountPtr;
   switch (type)
   {
   case AssetAccountTypeEnum_Plain:
   {
      accountPtr = make_shared<AssetAccount>(
         account_id, parent_id,
         rootEntry, derScheme,
         iface, dbName);
      break;
   }
   
   case AssetAccountTypeEnum_ECDH:
   {
      accountPtr = make_shared<AssetAccount_ECDH>(
         account_id, parent_id,
         rootEntry, derScheme,
         iface, dbName);
      break;
   }

   default:
      throw AccountException("unexpected asset account type");
   }

   //fill members not covered by the ctor
   accountPtr->lastUsedIndex_ = lastUsedIndex;
   accountPtr->assets_ = move(assetMap);

   return accountPtr;
}

////////////////////////////////////////////////////////////////////////////////
int AssetAccount::getLastComputedIndex(void) const
{
   if (getAssetCount() == 0)
      return -1;

   auto iter = assets_.rbegin();
   return iter->first;
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPublicChain(unsigned count)
{
   ReentrantLock lock(this);

   //add *count* entries to address chain
   shared_ptr<AssetEntry> assetPtr = nullptr;
   if (assets_.size() != 0)
      assetPtr = assets_.rbegin()->second;
   else
      assetPtr = root_;

   if (count == 0)
      return;

   extendPublicChain(assetPtr, count);
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPublicChainToIndex(unsigned count)
{
   ReentrantLock lock(this);

   //make address chain at least *count* long
   auto lastComputedIndex = max(getLastComputedIndex(), 0);
   if (lastComputedIndex > count)
      return;

   auto toCompute = count - lastComputedIndex;
   extendPublicChain(toCompute);
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPublicChain(
   shared_ptr<AssetEntry> assetPtr, unsigned count)
{
   if (count == 0)
      return;

   ReentrantLock lock(this);

   auto&& assetVec = extendPublicChain(assetPtr,
      assetPtr->getIndex() + 1, 
      assetPtr->getIndex() + count);

   {
      for (auto& asset : assetVec)
      {
         auto id = asset->getIndex();
         auto iter = assets_.find(id);
         if (iter != assets_.end())
            continue;

         assets_.insert(make_pair(
            id, asset));
      }
   }

   updateOnDiskAssets();
}

////////////////////////////////////////////////////////////////////////////////
vector<shared_ptr<AssetEntry>> AssetAccount::extendPublicChain(
   shared_ptr<AssetEntry> assetPtr, 
   unsigned start, unsigned end)
{
   vector<shared_ptr<AssetEntry>> result;

   switch (derScheme_->getType())
   {
   case DerSchemeType_ArmoryLegacy:
   {
      //Armory legacy derivation operates from the last valid asset
      result = move(derScheme_->extendPublicChain(assetPtr, start, end));
      break;
   }

   case DerSchemeType_BIP32:
   case DerSchemeType_ECDH:
   {
      //BIP32 operates from the node's root asset
      result = move(derScheme_->extendPublicChain(root_, start, end));
      break;
   }

   default:
      throw AccountException("unexpected derscheme type");
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPrivateChain(
   shared_ptr<DecryptedDataContainer> ddc,
   unsigned count)
{
   ReentrantLock lock(this);
   shared_ptr<AssetEntry> topAsset = nullptr;
   
   try
   {
      topAsset = getLastAssetWithPrivateKey();
   }
   catch(runtime_error&)
   {}

   extendPrivateChain(ddc, topAsset, count);
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPrivateChainToIndex(
   shared_ptr<DecryptedDataContainer> ddc,
   unsigned id)
{
   ReentrantLock lock(this);

   shared_ptr<AssetEntry> topAsset = nullptr;
   int topIndex = 0;

   try
   {
      topAsset = getLastAssetWithPrivateKey();
      topIndex = topAsset->getIndex();
   }
   catch(runtime_error&)
   {}

   if (id > topIndex)
   {
      auto count = id - topIndex;
      extendPrivateChain(ddc, topAsset, count);
   }
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::extendPrivateChain(
   shared_ptr<DecryptedDataContainer> ddc,
   shared_ptr<AssetEntry> assetPtr, unsigned count)
{
   if (count == 0)
      return;

   ReentrantLock lock(this);
   unsigned assetIndex = UINT32_MAX;
   if (assetPtr != nullptr)
      assetIndex = assetPtr->getIndex();

   auto&& assetVec = extendPrivateChain(ddc, assetPtr, 
      assetIndex + 1, assetIndex + count);

   {
      for (auto& asset : assetVec)
      {
         auto id = asset->getIndex();
         auto iter = assets_.find(id);
         if (iter != assets_.end())
         {
            if (iter->second->hasPrivateKey())
            {
               //do not overwrite an existing asset that already has a privkey
               continue;
            }
            else
            {
               iter->second = asset;
               continue;
            }
         }

         assets_.insert(make_pair(
            id, asset));
      }
   }

   updateOnDiskAssets();
}

////////////////////////////////////////////////////////////////////////////////
vector<shared_ptr<AssetEntry>> AssetAccount::extendPrivateChain(
   shared_ptr<DecryptedDataContainer> ddc,
   shared_ptr<AssetEntry> assetPtr,
   unsigned start, unsigned end)
{
   vector<shared_ptr<AssetEntry>> result;

   switch (derScheme_->getType())
   {
   case DerSchemeType_ArmoryLegacy:
   {
      //Armory legacy derivation operates from the last valid asset
      result = move(derScheme_->extendPrivateChain(ddc, assetPtr, start, end));
      break;
   }

   case DerSchemeType_BIP32:
   case DerSchemeType_ECDH:
   {
      //BIP32 operates from the node's root asset
      result = move(derScheme_->extendPrivateChain(ddc, root_, start, end));
      break;
   }

   default:
      throw AccountException("unexpected derscheme type");
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetAccount::getLastAssetWithPrivateKey() const
{
   ReentrantLock lock(this);

   auto assetIter = assets_.rbegin();
   while (assetIter != assets_.rend())
   {
      if (assetIter->second->hasPrivateKey())
         return assetIter->second;

      ++assetIter;
   }

   throw runtime_error("no asset with private keys");
   return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::updateHighestUsedIndex()
{
   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ASSET_TOP_INDEX_PREFIX);
   bwKey.put_BinaryData(getFullID());

   BinaryWriter bwData;
   bwData.put_var_int(lastUsedIndex_);

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->insert(bwKey.getData(), bwData.getData());
}

////////////////////////////////////////////////////////////////////////////////
unsigned AssetAccount::getAndBumpHighestUsedIndex()
{
   ReentrantLock lock(this);

   ++lastUsedIndex_;
   updateHighestUsedIndex();
   return lastUsedIndex_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetAccount::getNewAsset()
{
   ReentrantLock lock(this);

   auto index = getAndBumpHighestUsedIndex();
   auto entryIter = assets_.find(index);
   if (entryIter == assets_.end())
   {
      extendPublicChain(getLookup());
      entryIter = assets_.find(index);
      if (entryIter == assets_.end())
         throw AccountException("requested index overflows max lookup");
   }

   return entryIter->second;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetAccount::peekNextAsset()
{
   ReentrantLock lock(this);

   auto index = lastUsedIndex_ + 1;
   auto entryIter = assets_.find(index);
   if (entryIter == assets_.end())
   {
      extendPublicChain(getLookup());
      entryIter = assets_.find(index);
      if (entryIter == assets_.end())
         throw AccountException("requested index overflows max lookup");
   }

   return entryIter->second;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetAccount::getAssetForID(const BinaryData& ID) const
{
   if (ID.getSize() < 4)
      throw runtime_error("invalid asset ID");

   auto id_int = READ_UINT32_BE(ID.getPtr());
   return getAssetForIndex(id_int);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetAccount::getAssetForIndex(unsigned id) const
{
   auto iter = assets_.find(id);
   if (iter == assets_.end())
      throw AccountException("unknown asset index");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
void AssetAccount::updateAddressHashMap(
   const set<AddressEntryType>& typeSet)
{
   auto assetIter = assets_.find(lastHashedAsset_);
   if (assetIter == assets_.end())
   {
      assetIter = assets_.begin();
   }
   else
   {
      ++assetIter;
      if (assetIter == assets_.end())
         return;
   }

   ReentrantLock lock(this);

   while (assetIter != assets_.end())
   {
      auto hashMapiter = addrHashMap_.find(assetIter->second->getID());
      if (hashMapiter == addrHashMap_.end())
      {
         hashMapiter = addrHashMap_.insert(make_pair(
            assetIter->second->getID(),
            map<AddressEntryType, BinaryData>())).first;
      }

      for (auto ae_type : typeSet)
      {
         if (hashMapiter->second.find(ae_type) != hashMapiter->second.end())
            continue;

         auto addrPtr = AddressEntry::instantiate(assetIter->second, ae_type);
         auto& addrHash = addrPtr->getPrefixedHash();
         addrHashMap_[assetIter->second->getID()].insert(
            make_pair(ae_type, addrHash));
      }

      lastHashedAsset_ = assetIter->first;
      ++assetIter;
   }
}

////////////////////////////////////////////////////////////////////////////////
const map<BinaryData, map<AddressEntryType, BinaryData>>& 
   AssetAccount::getAddressHashMap(const set<AddressEntryType>& typeSet)
{
   updateAddressHashMap(typeSet);

   return addrHashMap_;
}

////////////////////////////////////////////////////////////////////////////////
const SecureBinaryData& AssetAccount::getChaincode() const
{
   if (derScheme_ == nullptr)
      throw AccountException("null derivation scheme");

   return derScheme_->getChaincode();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<Asset_PrivateKey> AssetAccount::fillPrivateKey(
   shared_ptr<DecryptedDataContainer> ddc,
   const BinaryData& id)
{
   if (id.getSize() != 12)
      throw AccountException("unexpected asset id length");

   //get the asset
   auto assetID_bdr = id.getSliceRef(8, 4);
   auto assetID = READ_UINT32_BE(assetID_bdr);

   auto iter = assets_.find(assetID);
   if (iter == assets_.end())
      throw AccountException("invalid asset id");

   auto thisAsset = std::dynamic_pointer_cast<AssetEntry_Single>(iter->second);
   if (thisAsset == nullptr)
      throw AccountException("unexpected asset type in map");

   //sanity check
   if (thisAsset->hasPrivateKey())
      return thisAsset->getPrivKey();

   //reverse iter through the map, find closest previous asset with priv key
   //this is only necessary for armory 1.35 derivation
   shared_ptr<AssetEntry> prevAssetWithKey = nullptr;
   map<unsigned, shared_ptr<AssetEntry>>::reverse_iterator rIter(iter);
   while (rIter != assets_.rend())
   {
      if (rIter->second->hasPrivateKey())
      {
         prevAssetWithKey = rIter->second;
         break;
      }

      ++rIter;
   }
   
   //if no asset in map had a private key, use the account root instead
   if (prevAssetWithKey == nullptr)
      prevAssetWithKey = root_;

   //figure out the asset count
   unsigned count = assetID - (unsigned)prevAssetWithKey->getIndex();

   //extend the private chain
   extendPrivateChain(ddc, prevAssetWithKey, count);

   //grab the fresh asset, return its private key
   auto privKeyIter = assets_.find(assetID);
   if (privKeyIter == assets_.end())
      throw AccountException("invalid asset id");

   if (!privKeyIter->second->hasPrivateKey())
      throw AccountException("fillPrivateKey failed");

   auto assetSingle = 
      std::dynamic_pointer_cast<AssetEntry_Single>(privKeyIter->second);
   if(assetSingle == nullptr)
      throw AccountException("fillPrivateKey failed");

   return assetSingle->getPrivKey();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AssetAccount_ECDH
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
unsigned AssetAccount_ECDH::addSalt(const SecureBinaryData& salt)
{
   auto derScheme = dynamic_pointer_cast<DerivationScheme_ECDH>(derScheme_);
   if (derScheme == nullptr)
      throw AccountException("unexpected derivation scheme type");

   return derScheme->addSalt(salt, iface_, dbName_);
}

////////////////////////////////////////////////////////////////////////////////
unsigned AssetAccount_ECDH::getSaltIndex(const SecureBinaryData& salt) const
{
   auto derScheme = dynamic_pointer_cast<DerivationScheme_ECDH>(derScheme_);
   if (derScheme == nullptr)
      throw AccountException("unexpected derivation scheme type");

   return derScheme->getSaltIndex(salt);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AddressAccount
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void AddressAccount::make_new(
   shared_ptr<AccountType> accType,
   shared_ptr<DecryptedDataContainer> decrData,
   unique_ptr<Cipher> cipher)
{
   reset();

   //create root asset
   auto createRootAsset = [&decrData, this](
      shared_ptr<AccountType_BIP32> accBip32,
      unsigned node_id, unique_ptr<Cipher> cipher_copy)->
      shared_ptr<AssetEntry_BIP32Root>
   {
      auto&& account_id = WRITE_UINT32_BE(node_id);
      auto&& full_account_id = ID_ + account_id;

      shared_ptr<AssetEntry_BIP32Root> rootAsset;
      SecureBinaryData chaincode;

      BIP32_Node node;

      if (accBip32->isWatchingOnly())
      {
         //WO
         node.initFromPublicKey(
            accBip32->getDepth(), accBip32->getLeafID(), accBip32->getFingerPrint(),
            accBip32->getPublicRoot(), accBip32->getChaincode());
         
         auto derPath = accBip32->getDerivationPath();
         
         //check AccountType_BIP32_Custom comments for more info
         if(node_id != UINT32_MAX)
         {
            node.derivePublic(node_id);
            derPath.push_back(node_id);
         }

         chaincode = node.moveChaincode();
         auto pubkey = node.movePublicKey();

         rootAsset = make_shared<AssetEntry_BIP32Root>(
            -1, full_account_id,
            pubkey, nullptr,
            chaincode,
            node.getDepth(), node.getLeafID(), 
            node.getParentFingerprint(), accBip32->getSeedFingerprint(),
            derPath);
      }
      else
      {
         //full wallet
         node.initFromPrivateKey(
            accBip32->getDepth(), accBip32->getLeafID(), accBip32->getFingerPrint(),
            accBip32->getPrivateRoot(), accBip32->getChaincode());

         auto derPath = accBip32->getDerivationPath();

         //check AccountType_BIP32_Custom comments for more info
         if (node_id != UINT32_MAX)
         {  
            node.derivePrivate(node_id);
            derPath.push_back(node_id);
         }

         chaincode = node.moveChaincode();
         
         auto pubkey = node.movePublicKey();
         if (pubkey.getSize() == 0)
         {
            auto&& pubkey_unc = 
               CryptoECDSA().ComputePublicKey(accBip32->getPrivateRoot());
            pubkey = move(CryptoECDSA().CompressPoint(pubkey_unc));
         }

         ReentrantLock lock(decrData.get());

         //encrypt private root
         auto&& encrypted_root =
            decrData->encryptData(cipher_copy.get(), node.getPrivateKey());

         //create assets
         auto privKeyID = full_account_id;
         privKeyID.append(WRITE_UINT32_LE(UINT32_MAX));
         auto priv_asset = make_shared<Asset_PrivateKey>(
            privKeyID, encrypted_root, move(cipher_copy));
         rootAsset = make_shared<AssetEntry_BIP32Root>(
            -1, full_account_id,
            pubkey, priv_asset,
            chaincode,
            node.getDepth(), node.getLeafID(), 
            node.getParentFingerprint(), accBip32->getSeedFingerprint(),
            derPath);
      }

      return rootAsset;
   };

   //create account
   auto createNewAccount = [this](
      shared_ptr<AssetEntry_BIP32Root> rootAsset,
      shared_ptr<DerivationScheme_BIP32> derScheme)->
      shared_ptr<AssetAccount>
   {
      if(rootAsset == nullptr)
         throw AccountException("null root asset");

      //der scheme
      if(derScheme == nullptr)
      {
         auto chaincode = rootAsset->getChaincode();
         if (chaincode.getSize() == 0)
            throw AccountException("invalid chaincode");

         derScheme = make_shared<DerivationScheme_BIP32>(
            chaincode, rootAsset->getDepth(), rootAsset->getLeafID());
      }

      //account id
      auto full_account_id = rootAsset->getAccountID();
      auto len = full_account_id.getSize();
      if (ID_.getSize() > len)
         throw AccountException("unexpected ID size");

      auto account_id = full_account_id.getSliceCopy(
         ID_.getSize(), len - ID_.getSize());

      //instantiate account
      auto asset_account = make_shared<AssetAccount>(
         account_id, ID_,
         rootAsset, derScheme,
         iface_, dbName_);

      return asset_account;
   };

   //body
   switch (accType->type())
   {
   case AccountTypeEnum_ArmoryLegacy:
   {
      auto accPtr = dynamic_pointer_cast<AccountType_ArmoryLegacy>(accType);
      ID_ = accPtr->getAccountID();
      auto asset_account_id = accPtr->getOuterAccountID();

      //chaincode has to be a copy cause the derscheme ctor moves it in
      auto chaincode = accPtr->getChaincode();
      auto derScheme = make_shared<DerivationScheme_ArmoryLegacy>(
         chaincode);

      //first derived asset
      auto&& full_account_id = ID_ + asset_account_id;
      shared_ptr<AssetEntry_Single> firstAsset;

      if (accPtr->isWatchingOnly())
      {
         //WO
         auto& root = accPtr->getPublicRoot();
         firstAsset = derScheme->computeNextPublicEntry(
            root,
            full_account_id, 0);
      }
      else
      {
         //full wallet
         ReentrantLock lock(decrData.get());
         
         auto& root = accPtr->getPrivateRoot();
         firstAsset = derScheme->computeNextPrivateEntry(
            decrData,
            root, move(cipher),
            full_account_id, 0);
      }

      //instantiate account and set first entry
      auto asset_account = make_shared<AssetAccount>(
         asset_account_id, ID_,
         //no root asset for legacy derivation scheme, using first entry instead
         nullptr, derScheme, 
         iface_, dbName_);
      asset_account->assets_.insert(make_pair(0, firstAsset));

      //add the asset account
      addAccount(asset_account);

      break;
   }

   case AccountTypeEnum_BIP32:
   case AccountTypeEnum_BIP32_Salted:
   {
      auto accBip32 = dynamic_pointer_cast<AccountType_BIP32>(accType);
      if (accBip32 == nullptr)
         throw AccountException("unexpected account type");

      ID_ = accBip32->getAccountID();

      auto nodes = accBip32->getNodes();
      if (nodes.size() > 0)
      {
         for (auto& node : nodes)
         {
            shared_ptr<AssetEntry_BIP32Root> root_obj;
            if (cipher != nullptr)
            {
               root_obj = createRootAsset(
                  accBip32, node,
                  move(cipher->getCopy()));
            }
            else
            {
               root_obj = createRootAsset(
                  accBip32, node,
                  nullptr);
            }
            
            shared_ptr<DerivationScheme_BIP32> derScheme = nullptr;
            if (accType->type() == AccountTypeEnum_BIP32_Salted)
            {
               auto accSalted = 
                  dynamic_pointer_cast<AccountType_BIP32_Salted>(accType);
               if (accSalted == nullptr)
                  throw AccountException("unexpected account type");

               if (accSalted->getSalt().getSize() != 32)
                  throw AccountException("invalid salt len");

               auto chaincode = root_obj->getChaincode();
               auto salt = accSalted->getSalt();
               derScheme = 
                  make_shared<DerivationScheme_BIP32_Salted>(
                     salt, chaincode, 
                     root_obj->getDepth(), root_obj->getLeafID());
            }

            auto account_obj = createNewAccount(
               root_obj, derScheme);
            addAccount(account_obj);
         }
      }
      else
      {
         shared_ptr<AssetEntry_BIP32Root> root_obj;
         if (cipher != nullptr)
         {
            root_obj = createRootAsset(
               accBip32, 
               //check AccountType_BIP32_Custom comments for more info
               UINT32_MAX, 
               move(cipher->getCopy()));
         }
         else
         {
            root_obj = createRootAsset(
               accBip32, 
               //check AccountType_BIP32_Custom comments for more info
               UINT32_MAX, 
               nullptr);
         }

         shared_ptr<DerivationScheme_BIP32> derScheme = nullptr;
         if (accType->type() == AccountTypeEnum_BIP32_Salted)
         {
            auto accSalted = 
               dynamic_pointer_cast<AccountType_BIP32_Salted>(accType);
            if (accSalted == nullptr)
               throw AccountException("unexpected account type");

            if (accSalted->getSalt().getSize() != 32)
               throw AccountException("invalid salt len");
               
            auto chaincode = root_obj->getChaincode();
            auto salt = accSalted->getSalt();
            derScheme = 
               make_shared<DerivationScheme_BIP32_Salted>(
                  salt, chaincode, 
                  root_obj->getDepth(), root_obj->getLeafID());
         }
            
         auto account_obj = createNewAccount(
            root_obj, derScheme);
         addAccount(account_obj);
      }

      break;
   }

   case AccountTypeEnum_ECDH:
   {
      auto accEcdh = dynamic_pointer_cast<AccountType_ECDH>(accType);
      if (accEcdh == nullptr)
         throw AccountException("unexpected account type");

      ID_ = accEcdh->getAccountID();

      //ids
      auto accountID = ID_;
      accountID.append(accEcdh->getOuterAccountID());

      //root asset
      shared_ptr<AssetEntry_Single> rootAsset;
      if (accEcdh->isWatchingOnly())
      {
         //WO
         auto pubkeyCopy = accEcdh->getPubKey();
         rootAsset = make_shared<AssetEntry_Single>(
            -1, accountID,
            pubkeyCopy, nullptr);
      }
      else
      {
         //full wallet
         auto pubkey = accEcdh->getPubKey();
         if (pubkey.getSize() == 0)
         {
            auto&& pubkey_unc =
               CryptoECDSA().ComputePublicKey(accEcdh->getPrivKey());
            pubkey = move(CryptoECDSA().CompressPoint(pubkey_unc));
         }

         ReentrantLock lock(decrData.get());

         //encrypt private root
         auto&& cipher_copy = cipher->getCopy();
         auto&& encrypted_root =
            decrData->encryptData(cipher_copy.get(), accEcdh->getPrivKey());

         //create assets
         auto privKeyID = accountID;
         privKeyID.append(WRITE_UINT32_LE(UINT32_MAX));
         auto priv_asset = make_shared<Asset_PrivateKey>(
            privKeyID, encrypted_root, move(cipher_copy));
         rootAsset = make_shared<AssetEntry_Single>(
            -1, accountID,
            pubkey, priv_asset);
      }

      //derivation scheme
      auto derScheme = make_shared<DerivationScheme_ECDH>();

      //account
      auto assetAccount = make_shared<AssetAccount_ECDH>(
         accEcdh->getOuterAccountID(), ID_,
         rootAsset, derScheme,
         iface_, dbName_);

      addAccount(assetAccount);
      break;
   }

   default:
      throw AccountException("unknown account type");
   }

   //set the address types
   addressTypes_ = accType->getAddressTypes();

   //set default address type
   defaultAddressEntryType_ = accType->getDefaultAddressEntryType();

   //set inner and outer accounts
   outerAccount_ = accType->getOuterAccountID();
   innerAccount_ = accType->getInnerAccountID();
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::reset()
{
   outerAccount_.clear();
   innerAccount_.clear();

   assetAccounts_.clear();
   addressTypes_.clear();
   addressHashes_.clear();
   ID_.clear();

   addresses_.clear();
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::commit()
{
   //id as key
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_ACCOUNT_PREFIX);
   bwKey.put_BinaryData(ID_);

   //data
   BinaryWriter bwData;

   //outer and inner account
   bwData.put_var_int(outerAccount_.getSize());
   bwData.put_BinaryData(outerAccount_);

   bwData.put_var_int(innerAccount_.getSize());
   bwData.put_BinaryData(innerAccount_);

   //address type set
   bwData.put_var_int(addressTypes_.size());

   for (auto& addrType : addressTypes_)
      bwData.put_uint32_t(addrType);

   //default address type
   bwData.put_uint32_t(defaultAddressEntryType_);

   //asset accounts count
   bwData.put_var_int(assetAccounts_.size());

   auto&& tx = iface_->beginWriteTransaction(dbName_);

   //asset accounts
   for (auto& account : assetAccounts_)
   {
      auto&& assetAccountID = account.second->getFullID();
      bwData.put_var_int(assetAccountID.getSize());
      bwData.put_BinaryData(assetAccountID);

      account.second->commit();
   }

   //commit address account data to disk
   tx->insert(bwKey.getData(), bwData.getData());

   //commit instantiated address types
   for (auto& addrPair : addresses_)
      writeAddressType(addrPair.first, addrPair.second);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::addAccount(shared_ptr<AssetAccount> account)
{
   auto& accID = account->getID();
   if (accID.getSize() != 4)
      throw AccountException("invalid account id length");

   auto insertPair = assetAccounts_.insert(make_pair(
      account->getID(), account));

   if (!insertPair.second)
      throw AccountException("already have this asset account");
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::readFromDisk(const BinaryData& key)
{
   //sanity checks
   if (key.getSize() == 0)
      throw AccountException("empty AddressAccount key");

   if (key.getPtr()[0] != ADDRESS_ACCOUNT_PREFIX)
      throw AccountException("unexpected key prefix for AddressAccount");

   if (iface_ == nullptr || dbName_.size() == 0)
      throw AccountException("unintialized AddressAccount object");

   //wipe object prior to loading from disk
   reset();

   //get data from disk  
   auto&& tx = iface_->beginReadTransaction(dbName_);
   auto&& diskDataRef = tx->getDataRef(key);
   BinaryRefReader brr(diskDataRef);

   //outer and inner accounts
   size_t len, count;
   
   len = brr.get_var_int();
   outerAccount_ = brr.get_BinaryData(len);

   len = brr.get_var_int();
   innerAccount_ = brr.get_BinaryData(len);

   //address type set
   count = brr.get_var_int();
   for (unsigned i = 0; i < count; i++)
      addressTypes_.insert(AddressEntryType(brr.get_uint32_t()));

   //default address type
   defaultAddressEntryType_ = AddressEntryType(brr.get_uint32_t());

   //asset accounts
   count = brr.get_var_int();

   for (unsigned i = 0; i < count; i++)
   {
      len = brr.get_var_int();
      BinaryWriter bw_asset_key(1 + len);
      bw_asset_key.put_uint8_t(ASSET_ACCOUNT_PREFIX);
      bw_asset_key.put_BinaryData(brr.get_BinaryData(len));

      auto accountPtr = AssetAccount::loadFromDisk(
         bw_asset_key.getData(), iface_, dbName_);
      assetAccounts_.insert(make_pair(accountPtr->id_, accountPtr));
   }

   ID_ = key.getSliceCopy(1, key.getSize() - 1);

   //instantiated address types
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(getID());
   auto keyBdr = bwKey.getDataRef();

   auto dbIter = tx->getIterator();
   dbIter->seek(bwKey.getData());
   while (dbIter->isValid())
   {
      auto&& key = dbIter->key();
      if (!key.startsWith(keyBdr))
         break;

      if (key.getSize() != 13)
      {
         LOGWARN << "unexpected address entry type key size!";
         dbIter->advance();
         continue;
      }

      auto&& data = dbIter->value();
      if (data.getSize() != 4)
      {
         LOGWARN << "unexpected address entry type val size!";
         dbIter->advance();
         continue;
      }

      auto aeType = AddressEntryType(*(uint32_t*)data.getPtr());
      auto assetID = key.getSliceCopy(1, 12);
      addresses_.insert(make_pair(assetID, aeType));
      
      dbIter->advance();
   }
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPublicChain(unsigned count)
{
   for (auto& account : assetAccounts_)
      account.second->extendPublicChain(count);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPrivateChain(
   shared_ptr<DecryptedDataContainer> ddc,
   unsigned count)
{
   for (auto& account : assetAccounts_)
      account.second->extendPrivateChain(ddc, count);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPublicChainToIndex(
   const BinaryData& accountID, unsigned count)
{
   auto iter = assetAccounts_.find(accountID);
   if (iter == assetAccounts_.end())
      throw AccountException("unknown account");

   iter->second->extendPublicChainToIndex(count);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPrivateChainToIndex(
   shared_ptr<DecryptedDataContainer> ddc,
   const BinaryData& accountID, unsigned count)
{
   auto iter = assetAccounts_.find(accountID);
   if (iter == assetAccounts_.end())
      throw AccountException("unknown account");

   iter->second->extendPrivateChainToIndex(ddc, count);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewAddress(
   AddressEntryType aeType)
{
   if (outerAccount_.getSize() == 0)
      throw AccountException("no currently active asset account");

   return getNewAddress(outerAccount_, aeType);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewChangeAddress(
   AddressEntryType aeType)
{
   if (innerAccount_.getSize() == 0)
      throw AccountException("no currently active asset account");

   return getNewAddress(innerAccount_, aeType);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::peekNextChangeAddress(
   AddressEntryType aeType)
{
   auto iter = assetAccounts_.find(innerAccount_);
   if (iter == assetAccounts_.end())
      throw AccountException("invalid asset account");

   if (aeType == AddressEntryType_Default)
      aeType = defaultAddressEntryType_;

   auto aeIter = addressTypes_.find(aeType);
   if (aeIter == addressTypes_.end())
      throw AccountException("invalid address type for this account");

   auto assetPtr = iter->second->getNewAsset();
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   
   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewAddress(
   const BinaryData& account, AddressEntryType aeType)
{
   auto iter = assetAccounts_.find(account);
   if (iter == assetAccounts_.end())
      throw AccountException("invalid asset account");

   if (aeType == AddressEntryType_Default)
      aeType = defaultAddressEntryType_;

   auto aeIter = addressTypes_.find(aeType);
   if (aeIter == addressTypes_.end())
      throw AccountException("invalid address type for this account");

   auto assetPtr = iter->second->getNewAsset();
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   
   //keep track of the address type for this asset if it doesnt use the 
   //account default
   if (aeType != defaultAddressEntryType_)
   {
      //update on disk
      updateInstantiatedAddressType(addrPtr);
   }

   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
bool AddressAccount::hasAddressType(AddressEntryType aeType)
{
   if (aeType == AddressEntryType_Default)
      return true;

   auto iter = addressTypes_.find(aeType);
   return iter != addressTypes_.end();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getAssetForID(const BinaryData& ID) const
{
   if (ID.getSize() != 8)
      throw AccountException("invalid asset ID");

   auto accID = ID.getSliceRef(0, 4);
   auto iter = assetAccounts_.find(accID);

   if (iter == assetAccounts_.end())
      throw AccountException("unknown account ID");

   auto assetID = ID.getSliceRef(4, ID.getSize() - 4);
   return iter->second->getAssetForID(assetID);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getAssetForID(unsigned ID, 
   bool outer) const
{
   BinaryDataRef accountID(outerAccount_);
   if (!outer)
      accountID.setRef(innerAccount_);

   auto iter = assetAccounts_.find(accountID);
   if (iter == assetAccounts_.end())
      throw AccountException("unknown account ID");

   return iter->second->getAssetForID(WRITE_UINT32_BE(ID));
}

////////////////////////////////////////////////////////////////////////////////
const pair<BinaryData, AddressEntryType>& 
   AddressAccount::getAssetIDPairForAddr(const BinaryData& scrAddr)
{
   updateAddressHashMap();

   auto iter = addressHashes_.find(scrAddr);
   if (iter == addressHashes_.end())
      throw AccountException("unknown scrAddr");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
const pair<BinaryData, AddressEntryType>& 
   AddressAccount::getAssetIDPairForAddrUnprefixed(const BinaryData& scrAddr)
{
   updateAddressHashMap();

   auto addressTypeSet = getAddressTypeSet();
   set<uint8_t> usedPrefixes;
   for (auto& addrType : addressTypeSet)
   {
      BinaryWriter bw;
      auto prefixByte = AddressEntry::getPrefixByte(addrType);
      auto insertIter = usedPrefixes.insert(prefixByte);
      if (!insertIter.second)
         continue;

      bw.put_uint8_t(prefixByte);
      bw.put_BinaryData(scrAddr);

      auto& addrData = bw.getData();
      auto iter = addressHashes_.find(addrData);
      if (iter == addressHashes_.end())
         continue;

      return iter->second;
   }

   throw AccountException("unknown scrAddr");
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateAddressHashMap()
{
   ReentrantLock lock(this);

   for (auto account : assetAccounts_)
   {
      auto& hashMap = account.second->getAddressHashMap(addressTypes_);
      if (hashMap.size() == 0)
         continue;

      map<BinaryData, map<AddressEntryType, BinaryData>>::const_iterator hashMapIter;

      auto idIter = topHashedAssetId_.find(account.first);
      if (idIter == topHashedAssetId_.end())
      {
         hashMapIter = hashMap.begin();
      }
      else
      {
         hashMapIter = hashMap.find(idIter->second);
         ++hashMapIter;

         if (hashMapIter == hashMap.end())
            continue;
      }

      while (hashMapIter != hashMap.end())
      {
         for (auto& hash : hashMapIter->second)
         {
            auto&& inner_pair = make_pair(hashMapIter->first, hash.first);
            auto&& outer_pair = make_pair(hash.second, move(inner_pair));
            addressHashes_.emplace(outer_pair);
         }

         ++hashMapIter;
      }

      topHashedAssetId_[account.first] = hashMap.rbegin()->first;
   }
}

////////////////////////////////////////////////////////////////////////////////
const map<BinaryData, pair<BinaryData, AddressEntryType>>& 
   AddressAccount::getAddressHashMap()
{
   updateAddressHashMap();
   return addressHashes_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetAccount> AddressAccount::getOuterAccount() const
{
   auto iter = assetAccounts_.find(outerAccount_);
   if (iter == assetAccounts_.end())
      throw AccountException("invalid outer account ID");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
const map<BinaryData, shared_ptr<AssetAccount>>& 
   AddressAccount::getAccountMap() const
{
   return assetAccounts_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getOutterAssetForIndex(unsigned id) const
{
   auto account = getOuterAccount();
   return account->getAssetForIndex(id);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getOutterAssetRoot() const
{
   auto account = getOuterAccount();
   return account->getRoot();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressAccount> AddressAccount::getWatchingOnlyCopy(
   shared_ptr<WalletDBInterface> iface, const string& dbName) const
{
   auto woAcc = make_shared<AddressAccount>(iface, dbName);

   //id
   woAcc->ID_ = ID_;

   //address
   woAcc->defaultAddressEntryType_ = defaultAddressEntryType_;
   woAcc->addressTypes_ = addressTypes_;
   woAcc->addresses_ = addresses_;

   //account ids
   woAcc->outerAccount_ = outerAccount_;
   woAcc->innerAccount_ = innerAccount_;

   //asset accounts
   for (auto& assetAccPair : assetAccounts_)
   {
      auto assetAccPtr = assetAccPair.second;

      shared_ptr<AssetEntry> woRoot = nullptr;
      if (assetAccPtr->root_ != nullptr)
      {
         /*
         Only check account root type if it has a root to begin with. Some
         accounts do not carry roots (e.g. from Armory135 wallets)
         */
         auto rootSingle = dynamic_pointer_cast<AssetEntry_Single>(assetAccPtr->root_);
         if (rootSingle == nullptr)
            throw AccountException("invalid account root");
         woRoot = rootSingle->getPublicCopy();
      }

      shared_ptr<AssetAccount> woAccPtr;
      switch (assetAccPtr->type())
      {
      case AssetAccountTypeEnum_Plain:
      {
         woAccPtr = make_shared<AssetAccount>(
            assetAccPtr->id_, assetAccPtr->parent_id_,
            woRoot,
            assetAccPtr->derScheme_,
            iface, dbName);
         break;
      }

      case AssetAccountTypeEnum_ECDH:
      {
         woAccPtr = make_shared<AssetAccount_ECDH>(
            assetAccPtr->id_, assetAccPtr->parent_id_,
            woRoot,
            assetAccPtr->derScheme_,
            iface, dbName);

         //put derScheme salts
         auto derSchemePtr = dynamic_pointer_cast<DerivationScheme_ECDH>(
            assetAccPtr->derScheme_);
         if (derSchemePtr == nullptr)
            throw AccountException("unexpected der scheme object type");
         derSchemePtr->putAllSalts(iface, dbName);

         break;
      }
      }

      woAccPtr->lastUsedIndex_ = assetAccPtr->lastUsedIndex_;

      for (auto& assetPair : assetAccPtr->assets_)
      {
         auto assetSingle = 
            dynamic_pointer_cast<AssetEntry_Single>(assetPair.second);
         if (assetSingle == nullptr)
            throw AccountException("unexpect asset type");

         auto assetWo = assetSingle->getPublicCopy();
         assetWo->flagForCommit();
         woAccPtr->assets_.insert(make_pair(assetPair.first, assetWo));
      }

      woAcc->addAccount(woAccPtr);
   }

   return woAcc;
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateInstantiatedAddressType(
   shared_ptr<AddressEntry> addrPtr)
{
   /***
   AddressAccount keeps track instantiated address types with a simple
   key-val scheme:

   (ADDRESS_PREFIX|Asset's ID):(AddressEntry type)

   Addresses using the account's default type are not recorded. Their type is
   infered on load by AssetAccounts' highest used index and the lack of explicit
   type entry.
   ***/

   //sanity check
   if (addrPtr->getType() == AddressEntryType_Default)
      throw AccountException("invalid address entry type");

   updateInstantiatedAddressType(addrPtr->getID(), addrPtr->getType());
}
  
////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateInstantiatedAddressType(
   const BinaryData& id, AddressEntryType aeType)
{
   //TODO: sanity check
   /*if (aeType != AddressEntryType_Default)
   {
      auto typeIter = addressTypes_.find(aeType);
      if (typeIter == addressTypes_.end())
         throw AccountException("invalid address type");
   }*/

   auto iter = addresses_.find(id);
   if (iter != addresses_.end())
   {
      //skip if type entry already exist and new type matches old one
      if (iter->second == aeType)
         return;

      //delete entry if new type matches default account type
      if (aeType == defaultAddressEntryType_)
      {
         addresses_.erase(iter);
         eraseInstantiatedAddressType(id);
         return;
      }
   }

   //otherwise write address type to disk
   addresses_[id] = aeType;
   writeAddressType(id, aeType);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::writeAddressType(
   const BinaryData& id, AddressEntryType aeType)
{
   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(id);

   BinaryWriter bwData;
   bwData.put_uint32_t(aeType);

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->insert(bwKey.getData(), bwData.getData());
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::eraseInstantiatedAddressType(const BinaryData& id)
{
   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(id);

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->erase(bwKey.getData());
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getAddressEntryForID(
   const BinaryDataRef& ID) const
{
   //sanity check
   if (ID.getSize() != 12)
      throw AccountException("invalid asset id");

   //get the asset account
   auto accIDRef = ID.getSliceRef(4, 4);
   auto accIter = assetAccounts_.find(accIDRef);
   if (accIter == assetAccounts_.end())
      throw AccountException("unknown account id");

   //does this ID exist?
   BinaryRefReader brr(ID);
   brr.advance(8);
   auto id_int = brr.get_uint32_t(BE);

   if (id_int > accIter->second->getHighestUsedIndex())
      throw UnrequestedAddressException();

   AddressEntryType aeType = defaultAddressEntryType_;
   //is there an address entry with this ID?
   auto addrIter = addresses_.find(ID);
   if (addrIter != addresses_.end())
      aeType = addrIter->second;

   auto assetPtr = accIter->second->getAssetForIndex(id_int);
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
map<BinaryData, shared_ptr<AddressEntry>> AddressAccount::getUsedAddressMap()
   const
{
   /***
   Expensive call, as addresses are built on the fly
   ***/

   map<BinaryData, shared_ptr<AddressEntry>> result;

   for (auto& account : assetAccounts_)
   {
      auto usedIndex = account.second->getHighestUsedIndex();
      if (usedIndex == UINT32_MAX)
         continue;

      for (unsigned i = 0; i <= usedIndex; i++)
      {
         auto assetPtr = account.second->getAssetForIndex(i);
         auto& assetID = assetPtr->getID();

         shared_ptr<AddressEntry> addrPtr;
         auto iter = addresses_.find(assetID);
         if (iter == addresses_.end())
            addrPtr = AddressEntry::instantiate(assetPtr, defaultAddressEntryType_);
         else
            addrPtr = AddressEntry::instantiate(assetPtr, iter->second);

         result.insert(make_pair(assetID, addrPtr));
      }
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<Asset_PrivateKey> AddressAccount::fillPrivateKey(
   shared_ptr<DecryptedDataContainer> ddc,
   const BinaryData& id)
{
   if (id.getSize() != 12)
      throw AccountException("invalid asset id");

   auto accID = id.getSliceRef(4, 4);
   auto iter = assetAccounts_.find(accID);
   if (iter == assetAccounts_.end())
      throw AccountException("unknown asset id");

   return iter->second->fillPrivateKey(ddc, id);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry_BIP32Root> AddressAccount::getBip32RootForAssetId(
   const BinaryData& assetId) const
{
   //sanity check
   if (assetId.getSize() != 12)
      throw AccountException("invalid asset id");

   //get the asset account
   auto accID = assetId.getSliceRef(4, 4);
   auto iter = assetAccounts_.find(accID);
   if (iter == assetAccounts_.end())
      throw AccountException("unknown asset id");

   //grab the account's root
   auto root = iter->second->root_;

   //is it bip32?
   auto rootBip32 = dynamic_pointer_cast<AssetEntry_BIP32Root>(root);
   if (rootBip32 == nullptr)
      throw AccountException("account isn't bip32");

   return rootBip32;
}

////////////////////////////////////////////////////////////////////////////////
bool AddressAccount::hasBip32Path(
   const ArmorySigner::BIP32_AssetPath& path) const
{
   //look for an account which root's path matches that of our desired path
   for (const auto& accountPair : assetAccounts_)
   {
      const auto& accountPtr = accountPair.second;
      
      auto root = accountPtr->root_;
      auto rootBip32 = dynamic_pointer_cast<AssetEntry_BIP32Root>(root);
      if (rootBip32 == nullptr)
         continue;

      auto rootPath = rootBip32->getDerivationPath();
      auto assetPath = path.getDerivationPathFromSeed();
      if (rootPath.empty() || 
         (rootPath.size() > assetPath.size()))
      {
         continue;
      }

      if (rootBip32->getSeedFingerprint(true) != path.getSeedFingerprint())
         return false;

      bool match = true;
      for (unsigned i=0; i<rootPath.size(); i++)
      {
         if (rootPath[i] != assetPath[i])
         {
            match = false;
            break;
         }
      }

      if (match)
         return true;
   }

   return false;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AccountType
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
AccountType::~AccountType()
{}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AccountType_WithRoot
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
AccountType_WithRoot::~AccountType_WithRoot()
{}

////////////////////////////////////////////////////////////////////////////////
void AccountType::setAddressTypes(
   const std::set<AddressEntryType>& addrTypeSet)
{
   addressTypes_ = addrTypeSet;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType::setDefaultAddressType(AddressEntryType addrType)
{
   defaultAddressEntryType_ = addrType;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AccountType_ArmoryLegacy
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
const SecureBinaryData& AccountType_ArmoryLegacy::getChaincode() const
{
   if (chainCode_.getSize() == 0)
   {
      auto& root = getPrivateRoot();
      if (root.getSize() == 0)
         throw AssetException("cannot derive chaincode from empty root");

      chainCode_ = move(BtcUtils::computeChainCode_Armory135(root));
   }

   return chainCode_;
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_ArmoryLegacy::getOuterAccountID(void) const
{
   return WRITE_UINT32_BE(ARMORY_LEGACY_ASSET_ACCOUNTID);
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_ArmoryLegacy::getInnerAccountID(void) const
{
   return WRITE_UINT32_BE(ARMORY_LEGACY_ASSET_ACCOUNTID);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AccountType_BIP32
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_BIP32::getAccountID() const
{
   //this ensures address accounts of different types based on the same
   //bip32 root do not end up with the same id

   BinaryWriter bw;
   bw.put_BinaryData(getPublicRoot());
   if (bw.getSize() == 0)
      throw AccountException("empty public root");

   //add in unique data identifying this account

   //account soft derivation paths
   for (auto& node : nodes_)
      bw.put_uint32_t(node, BE);
   
   //accounts structure
   if (!outerAccount_.empty())
      bw.put_BinaryData(outerAccount_);
   
   if (!innerAccount_.empty())
      bw.put_BinaryData(innerAccount_);

   //address types
   for (auto& addressType : addressTypes_)
      bw.put_uint32_t(addressType, BE);
   
   //default address
   bw.put_uint32_t(defaultAddressEntryType_);

   //main flag
   bw.put_uint8_t(isMain_);

   //hash, use first 4 bytes
   auto&& pub_hash160 = BtcUtils::getHash160(bw.getData());
   auto accountID = move(pub_hash160.getSliceCopy(0, 4));

   if (accountID == WRITE_UINT32_BE(ARMORY_LEGACY_ACCOUNTID) ||
       accountID == WRITE_UINT32_BE(IMPORTS_ACCOUNTID))
      throw AccountException("BIP32 account ID collision");

   return accountID;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::addAddressType(AddressEntryType addrType)
{
   addressTypes_.insert(addrType);
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setDefaultAddressType(AddressEntryType addrType)
{
   defaultAddressEntryType_ = addrType;
}

void AccountType_BIP32::setNodes(const std::set<unsigned>& nodes)
{
   nodes_ = nodes;
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_BIP32::getOuterAccountID(void) const
{
   if (outerAccount_.getSize() > 0)
      return outerAccount_;

   return WRITE_UINT32_BE(UINT32_MAX);
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_BIP32::getInnerAccountID(void) const
{
   if (innerAccount_.getSize() > 0)
      return innerAccount_;

   return WRITE_UINT32_BE(UINT32_MAX);
}

////////////////////////////////////////////////////////////////////////////////
unsigned AccountType_BIP32::getAddressLookup() const
{
   if (addressLookup_ == UINT32_MAX)
      throw AccountException("uninitialized address lookup");
   return addressLookup_;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setOuterAccountID(const BinaryData& outerAccount)
{
   outerAccount_ = outerAccount;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setInnerAccountID(const BinaryData& innerAccount)
{
   innerAccount_ = innerAccount;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setPrivateKey(const SecureBinaryData& key)
{
   privateRoot_ = key;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setPublicKey(const SecureBinaryData& key)
{
   publicRoot_ = key;
}

////////////////////////////////////////////////////////////////////////////////
void AccountType_BIP32::setChaincode(const SecureBinaryData& key)
{
   chainCode_ = key;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AccountType_ECDH
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
bool AccountType_ECDH::isWatchingOnly(void) const
{
   return privateKey_.empty();
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AccountType_ECDH::getAccountID() const
{
   BinaryData accountID;
   if (isWatchingOnly())
   {
      //this ensures address accounts of different types based on the same
      //bip32 root do not end up with the same id
      auto rootCopy = publicKey_;
      rootCopy.getPtr()[0] ^= (uint8_t)type();

      auto&& pub_hash160 = BtcUtils::getHash160(rootCopy);
      accountID = move(pub_hash160.getSliceCopy(0, 4));
   }
   else
   {
      auto&& root_pub = CryptoECDSA().ComputePublicKey(privateKey_);
      root_pub.getPtr()[0] ^= (uint8_t)type();

      auto&& pub_hash160 = BtcUtils::getHash160(root_pub);
      accountID = move(pub_hash160.getSliceCopy(0, 4));
   }

   if (accountID == WRITE_UINT32_BE(ARMORY_LEGACY_ACCOUNTID) ||
      accountID == WRITE_UINT32_BE(IMPORTS_ACCOUNTID))
      throw AccountException("BIP32 account ID collision");

   return accountID;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// MetaDataAccount
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::make_new(MetaAccountType type)
{
   type_ = type;

   switch (type_)
   {
   case MetaAccount_Comments:
   {
      ID_ = WRITE_UINT32_BE(META_ACCOUNT_COMMENTS);
      break;
   }

   case MetaAccount_AuthPeers:
   {
      ID_ = WRITE_UINT32_BE(META_ACCOUNT_AUTHPEER);
      break;
   }

   default:
      throw AccountException("unexpected meta account type");
   }
}

////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::commit()
{
   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(META_ACCOUNT_PREFIX);
   bwKey.put_BinaryData(ID_);

   BinaryWriter bwData;
   bwData.put_var_int(4);
   bwData.put_uint32_t((uint32_t)type_);

   //commit assets
   for (auto& asset : assets_)
      writeAssetToDisk(asset.second);

   //commit serialized account data
   auto&& tx = iface_->beginWriteTransaction(dbName_);
   tx->insert(bwKey.getData(), bwData.getData());
}

////////////////////////////////////////////////////////////////////////////////
bool MetaDataAccount::writeAssetToDisk(shared_ptr<MetaData> assetPtr)
{
   if (!assetPtr->needsCommit())
      return true;
   
   assetPtr->needsCommit_ = false;

   auto&& key = assetPtr->getDbKey();
   auto&& data = assetPtr->serialize();

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   if (data.getSize() != 0)
   {
      tx->insert(key, data);
      return true;
   }
   else
   {
      tx->erase(key);
      return false;
   }
}

////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::updateOnDisk(void)
{
   ReentrantLock lock(this);

   bool needsCommit = false;
   for (auto& asset : assets_)
      needsCommit |= asset.second->needsCommit();

   if (!needsCommit)
      return;

   auto&& tx = iface_->beginWriteTransaction(dbName_);
   auto iter = assets_.begin();
   while (iter != assets_.end())
   {
      if (writeAssetToDisk(iter->second))
      {
         ++iter;
         continue;
      }

      assets_.erase(iter++);
   }
}

////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::reset()
{
   type_ = MetaAccount_Unset;
   ID_.clear();
   assets_.clear();
}

////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::readFromDisk(const BinaryData& key)
{
   //sanity checks
   if (iface_ == nullptr || dbName_.size() == 0)
      throw AccountException("invalid db pointers");

   if (key.getSize() != 5)
      throw AccountException("invalid key size");

   if (key.getPtr()[0] != META_ACCOUNT_PREFIX)
      throw AccountException("unexpected prefix for AssetAccount key");

   auto&& tx = iface_->beginReadTransaction(dbName_);

   CharacterArrayRef carKey(key.getSize(), key.getCharPtr());

   auto diskDataRef = tx->getDataRef(key);
   BinaryRefReader brr(diskDataRef);

   //wipe object prior to loading from disk
   reset();

   //set ID
   ID_ = key.getSliceCopy(1, 4);

   //getType
   brr.get_var_int();
   type_ = (MetaAccountType)brr.get_uint32_t();

   uint8_t prefix;
   switch (type_)
   {
   case MetaAccount_Comments:
   {
      prefix = METADATA_COMMENTS_PREFIX;
      break;
   }

   case MetaAccount_AuthPeers:
   {
      prefix = METADATA_AUTHPEER_PREFIX;
      break;
   }

   default:
      throw AccountException("unexpected meta account type");
   }

   //get assets
   BinaryWriter bwAssetKey;
   bwAssetKey.put_uint8_t(prefix);
   bwAssetKey.put_BinaryData(ID_);
   auto& assetDbKey = bwAssetKey.getData();

   auto dbIter = tx->getIterator();
   dbIter->seek(assetDbKey);

   while (dbIter->isValid())
   {
      auto&& key = dbIter->key();
      auto&& data = dbIter->value();

      //check key isnt prefix
      if (key == assetDbKey)
         continue;

      //check key starts with prefix
      if (!key.startsWith(assetDbKey))
         break;

      //deser asset
      try
      {
         auto assetPtr = MetaData::deserialize(key, data);
         assets_.insert(make_pair(
            assetPtr->index_, assetPtr));
      }
      catch (exception&)
      {}

      dbIter->advance();
   }
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<MetaData> MetaDataAccount::getMetaDataByIndex(unsigned id) const
{
   auto iter = assets_.find(id);
   if (iter == assets_.end())
      throw AccountException("invalid asset index");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
void MetaDataAccount::eraseMetaDataByIndex(unsigned id)
{
   auto iter = assets_.find(id);
   if (iter == assets_.end())
      return;

   iter->second->clear();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<MetaDataAccount> MetaDataAccount::copy(
   shared_ptr<WalletDBInterface> iface, const string& dbName) const
{
   auto copyPtr = make_shared<MetaDataAccount>(iface, dbName);
   
   copyPtr->type_ = type_;
   copyPtr->ID_ = ID_;

   for (auto& assetPair : assets_)
   {
      auto assetCopy = assetPair.second->copy();
      assetCopy->flagForCommit();
      copyPtr->assets_.insert(make_pair(assetPair.first, assetCopy));
   }

   return copyPtr;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AuthPeerAssetConversion
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
AuthPeerAssetMap AuthPeerAssetConversion::getAssetMap(
   const MetaDataAccount* account)
{
   if (account == nullptr || account->type_ != MetaAccount_AuthPeers)
      throw AccountException("invalid metadata account ptr");
   ReentrantLock lock(account);

   AuthPeerAssetMap result;

   for (auto& asset : account->assets_)
   {
      switch (asset.second->type())
      {
      case MetaType_AuthorizedPeer:
      {
         auto assetPeer = dynamic_pointer_cast<PeerPublicData>(asset.second);
         if (assetPeer == nullptr)
            continue;

         auto& names = assetPeer->getNames();
         auto& pubKey = assetPeer->getPublicKey();

         for (auto& name : names)
            result.nameKeyPair_.emplace(make_pair(name, &pubKey));

         break;
      }

      case MetaType_PeerRootKey:
      {
         auto assetRoot = dynamic_pointer_cast<PeerRootKey>(asset.second);
         if (assetRoot == nullptr)
            continue;

         auto descPair = make_pair(assetRoot->getDescription(), asset.first);
         result.peerRootKeys_.emplace(make_pair(assetRoot->getKey(), descPair));
         
         break;
      }

      case MetaType_PeerRootSig:
      {
         auto assetSig = dynamic_pointer_cast<PeerRootSignature>(asset.second);
         if (assetSig == nullptr)
            continue;

         result.rootSignature_ = make_pair(assetSig->getKey(), assetSig->getSig());
      }

      default:
         continue;
      }
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
map<SecureBinaryData, set<unsigned>> 
   AuthPeerAssetConversion::getKeyIndexMap(const MetaDataAccount* account)
{
   if (account == nullptr || account->type_ != MetaAccount_AuthPeers)
      throw AccountException("invalid metadata account ptr");
   ReentrantLock lock(account);

   map<SecureBinaryData, set<unsigned>> result;

   for (auto& asset : account->assets_)
   {
      auto assetPeer = dynamic_pointer_cast<PeerPublicData>(asset.second);
      if (assetPeer == nullptr)
         throw AccountException("invalid asset type");

      auto& pubKey = assetPeer->getPublicKey();

      auto iter = result.find(pubKey);
      if (iter == result.end())
      {
         auto insertIter = result.insert(make_pair(
            pubKey, set<unsigned>()));
         iter = insertIter.first;
      }

      iter->second.insert(asset.first);
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
int AuthPeerAssetConversion::addAsset(
   MetaDataAccount* account, const SecureBinaryData& pubkey,
   const std::vector<std::string>& names)
{
   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_AuthPeers)
      throw AccountException("invalid metadata account ptr");

   auto& accountID = account->ID_;
   unsigned index = account->assets_.size();

   auto metaObject = make_shared<PeerPublicData>(accountID, index);
   metaObject->setPublicKey(pubkey);
   for (auto& name : names)
      metaObject->addName(name);

   metaObject->flagForCommit();
   account->assets_.emplace(make_pair(index, metaObject));
   account->updateOnDisk();

   return index;
}

////////////////////////////////////////////////////////////////////////////////
void AuthPeerAssetConversion::addRootSignature(MetaDataAccount* account,
   const SecureBinaryData& key, const SecureBinaryData& sig)
{
   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_AuthPeers)
      throw AccountException("invalid metadata account ptr");

   auto& accountID = account->ID_;
   unsigned index = account->assets_.size();

   auto metaObject = make_shared<PeerRootSignature>(accountID, index);
   metaObject->set(key, sig);
   
   metaObject->flagForCommit();
   account->assets_.emplace(make_pair(index, metaObject));
   account->updateOnDisk();
}
////////////////////////////////////////////////////////////////////////////////
unsigned AuthPeerAssetConversion::addRootPeer(MetaDataAccount* account,
   const SecureBinaryData& key, const std::string& desc)
{
   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_AuthPeers)
      throw AccountException("invalid metadata account ptr");

   auto& accountID = account->ID_;
   unsigned index = account->assets_.size();

   auto metaObject = make_shared<PeerRootKey>(accountID, index);
   metaObject->set(desc, key);

   metaObject->flagForCommit();
   account->assets_.emplace(make_pair(index, metaObject));
   account->updateOnDisk();

   return index;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// CommentAssetConversion
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
shared_ptr<CommentData> CommentAssetConversion::getByKey(
   MetaDataAccount* account, const BinaryData& key)
{
   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_Comments)
      throw AccountException("invalid metadata account ptr");

   for (auto& asset : account->assets_)
   {
      auto objPtr = dynamic_pointer_cast<CommentData>(asset.second);
      if (objPtr == nullptr)
         continue;

      if (objPtr->getKey() == key)
         return objPtr;
   }

   return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
int CommentAssetConversion::setAsset(MetaDataAccount* account,
   const BinaryData& key, const std::string& comment)
{
   if (comment.size() == 0)
      return INT32_MIN;

   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_Comments)
      throw AccountException("invalid metadata account ptr");

   auto metaObject = getByKey(account, key);

   if (metaObject == nullptr)
   {
      auto& accountID = account->ID_;
      auto index = (uint32_t)account->assets_.size();
      metaObject = make_shared<CommentData>(accountID, index);
      metaObject->setKey(key);

      account->assets_.emplace(make_pair(index, metaObject));
   }

   metaObject->setValue(comment);

   metaObject->flagForCommit();
   account->updateOnDisk();

   return metaObject->getIndex();
}

////////////////////////////////////////////////////////////////////////////////
int CommentAssetConversion::deleteAsset(
   MetaDataAccount* account, const BinaryData& key)
{
   auto metaObject = getByKey(account, key);
   if (metaObject == nullptr)
      return -1;

   metaObject->clear();
   account->updateOnDisk();

   return metaObject->getIndex();
}

////////////////////////////////////////////////////////////////////////////////
map<BinaryData, string> CommentAssetConversion::getCommentMap(
   MetaDataAccount* account)
{
   ReentrantLock lock(account);

   if (account == nullptr || account->type_ != MetaAccount_Comments)
      throw AccountException("invalid metadata account ptr");

   map<BinaryData, string> result;
   for (auto& asset : account->assets_)
   {
      auto objPtr = dynamic_pointer_cast<CommentData>(asset.second);
      if (objPtr == nullptr)
         continue;

      result.insert(make_pair(objPtr->getKey(), objPtr->getValue()));
   }

   return result;
}