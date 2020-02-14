////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//                                                                            //
//  Copyright (C) 2016-17, goatpig                                            //            
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                   
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "TestUtils.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
class PRNGTest : public ::testing::Test
{
protected:

   virtual void SetUp()
   {}

   virtual void TearDown()
   {}
};

////////////////////////////////////////////////////////////////////////////////
TEST_F(PRNGTest, FortunaTest)
{
   unsigned sampleSize = 1000000;

   auto checkPools = [](
      const set<SecureBinaryData>& p1,
      const set<SecureBinaryData>& p2, 
      size_t sampleSize, size_t len)
      ->vector<unsigned>
   {
      unsigned collisionP1 = 0;
      unsigned collisionP2 = 0;
      unsigned collisions = 0;
      unsigned offSizes = 0;
      if (p1.size() != sampleSize)
         collisionP1 = sampleSize - p1.size();

      if (p2.size() != sampleSize)
         collisionP2 = sampleSize - p2.size();

      for (auto& data : p1)
      {
         if (data.getSize() != len)
            ++offSizes;
         
         auto iter = p2.find(data);
         if (iter != p2.end())
            ++collisions;
      }

      for (auto& data : p2)
      {
         if (data.getSize() != len)
            ++offSizes;
      }

      return { collisionP1, collisionP2, collisions, offSizes };
   };

   PRNG_Fortuna prng1;
   PRNG_Fortuna prng2;

   //conscutive
   set<SecureBinaryData> pool1, pool2;
   for (unsigned i=0; i<sampleSize; i++)
      pool1.insert(prng1.generateRandom(32));

   for (unsigned i=0; i<sampleSize; i++)
      pool2.insert(prng2.generateRandom(32));

   auto check1 = checkPools(pool1, pool2, sampleSize, 32);
   EXPECT_EQ(check1[0], 0);
   EXPECT_EQ(check1[1], 0);
   EXPECT_EQ(check1[2], 0);
   EXPECT_EQ(check1[3], 0);

   //interlaced
   set<SecureBinaryData> pool3, pool4;
   auto thread2 = [&pool4, &prng2, &sampleSize]
   {
      for (unsigned i=0; i<sampleSize; i++)
         pool4.insert(prng2.generateRandom(32));
   };

   thread thr2(thread2);

   for (unsigned i=0; i<sampleSize; i++)
      pool3.insert(prng1.generateRandom(32));

   thr2.join();

   auto check2 = checkPools(pool3, pool4, sampleSize, 32);
   EXPECT_EQ(check2[0], 0);
   EXPECT_EQ(check2[1], 0);
   EXPECT_EQ(check2[2], 0);
   EXPECT_EQ(check2[3], 0);

   //cross checks
   auto check3 = checkPools(pool1, pool3, sampleSize, 32);
   EXPECT_EQ(check3[0], 0);
   EXPECT_EQ(check3[1], 0);
   EXPECT_EQ(check3[2], 0);
   EXPECT_EQ(check3[3], 0);

   auto check4 = checkPools(pool1, pool4, sampleSize, 32);
   EXPECT_EQ(check4[0], 0);
   EXPECT_EQ(check4[1], 0);
   EXPECT_EQ(check4[2], 0);
   EXPECT_EQ(check4[3], 0);

   auto check5 = checkPools(pool2, pool3, sampleSize, 32);
   EXPECT_EQ(check5[0], 0);
   EXPECT_EQ(check5[1], 0);
   EXPECT_EQ(check5[2], 0);
   EXPECT_EQ(check5[3], 0);

   auto check6 = checkPools(pool2, pool4, sampleSize, 32);
   EXPECT_EQ(check6[0], 0);
   EXPECT_EQ(check6[1], 0);
   EXPECT_EQ(check6[2], 0);
   EXPECT_EQ(check6[3], 0);

   //odd size pulls
   set<SecureBinaryData> pool5, pool6;
   for (unsigned i=0; i<100; i++)
      pool5.insert(prng1.generateRandom(15));

   for (unsigned i=0; i<100; i++)
      pool6.insert(prng2.generateRandom(15));

   auto check7 = checkPools(pool5, pool6, 100, 15);
   EXPECT_EQ(check7[0], 0);
   EXPECT_EQ(check7[1], 0);
   EXPECT_EQ(check7[2], 0);
   EXPECT_EQ(check7[3], 0);

   //
   set<SecureBinaryData> pool7, pool8;
   for (unsigned i=0; i<100; i++)
      pool7.insert(prng1.generateRandom(70));

   for (unsigned i=0; i<100; i++)
      pool8.insert(prng2.generateRandom(70));

   auto check8 = checkPools(pool7, pool8, 100, 70);
   EXPECT_EQ(check8[0], 0);
   EXPECT_EQ(check8[1], 0);
   EXPECT_EQ(check8[2], 0);
   EXPECT_EQ(check8[3], 0);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
class SignerTest : public ::testing::Test
{
protected:
   BlockDataManagerThread *theBDMt_ = nullptr;
   Clients* clients_ = nullptr;

   void initBDM(void)
   {
      DBTestUtils::init();
      auto& magicBytes = NetworkConfig::getMagicBytes();

      auto nodePtr = make_shared<NodeUnitTest>(
         *(uint32_t*)magicBytes.getPtr(), false);
      auto watcherPtr = make_shared<NodeUnitTest>(
         *(uint32_t*)magicBytes.getPtr(), true);
      config.bitcoinNodes_ = make_pair(nodePtr, watcherPtr);
      config.rpcNode_ = make_shared<NodeRPC_UnitTest>(nodePtr);
      
      theBDMt_ = new BlockDataManagerThread(config);
      iface_ = theBDMt_->bdm()->getIFace();

      nodePtr->setBlockchain(theBDMt_->bdm()->blockchain());
      nodePtr->setBlockFiles(theBDMt_->bdm()->blockFiles());

      auto mockedShutdown = [](void)->void {};
      clients_ = new Clients(theBDMt_, mockedShutdown);
   }

   /////////////////////////////////////////////////////////////////////////////
   virtual void SetUp()
   {
      LOGDISABLESTDOUT();
      ghash_ = READHEX(MAINNET_GENESIS_HASH_HEX);
      gentx_ = READHEX(MAINNET_GENESIS_TX_HASH_HEX);
      zeros_ = READHEX("00000000");

      blkdir_ = string("./blkfiletest");
      homedir_ = string("./fakehomedir");
      ldbdir_ = string("./ldbtestdir");

      DBUtils::removeDirectory(blkdir_);
      DBUtils::removeDirectory(homedir_);
      DBUtils::removeDirectory(ldbdir_);

      mkdir(blkdir_);
      mkdir(homedir_);
      mkdir(ldbdir_);

      BlockDataManagerConfig::setServiceType(SERVICE_UNITTEST);
      BlockDataManagerConfig::setOperationMode(OPERATION_UNITTEST);

      // Put the first 5 blocks into the blkdir
      blk0dat_ = BtcUtils::getBlkFilename(blkdir_, 0);
      TestUtils::setBlocks({ "0", "1", "2", "3", "4", "5" }, blk0dat_);

      BlockDataManagerConfig::setDbType(ARMORY_DB_BARE);
      config.blkFileLocation_ = blkdir_;
      config.dbDir_ = ldbdir_;
      config.threadCount_ = 3;

      NetworkConfig::selectNetwork(NETWORK_MODE_MAINNET);

      wallet1id = "wallet1";
      wallet2id = "wallet2";
      LB1ID = TestChain::lb1B58ID;
      LB2ID = TestChain::lb2B58ID;
   }

   /////////////////////////////////////////////////////////////////////////////
   virtual void TearDown(void)
   {
      if (clients_ != nullptr)
      {
         clients_->exitRequestLoop();
         clients_->shutdown();
      }

      delete clients_;
      delete theBDMt_;

      theBDMt_ = nullptr;
      clients_ = nullptr;

      DBUtils::removeDirectory(blkdir_);
      DBUtils::removeDirectory(homedir_);
      DBUtils::removeDirectory("./ldbtestdir");

      mkdir("./ldbtestdir");

      LOGENABLESTDOUT();
      CLEANUP_ALL_TIMERS();
   }

   BlockDataManagerConfig config;

   LMDBBlockDatabase* iface_;
   BinaryData ghash_;
   BinaryData gentx_;
   BinaryData zeros_;

   string blkdir_;
   string homedir_;
   string ldbdir_;
   string blk0dat_;

   string wallet1id;
   string wallet2id;
   string LB1ID;
   string LB2ID;
};

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, DISABLED_CheckChain_Test)
{
   //this test fails because the p2sh tx in our unit test chain are botched
   //(the input script has opcode when it should only be push data)

   config.threadCount_ = 1;
   config.checkChain_ = true;

   BlockDataManager bdm(config);

   try
   {
      bdm.doInitialSyncOnLoad(TestUtils::nullProgress);
   }
   catch (exception&)
   {
      //signify the failure
      EXPECT_TRUE(false);
   }

   EXPECT_EQ(bdm.getCheckedTxCount(), 20);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, Signer_Test)
{
   TestUtils::setBlocks({ "0", "1", "2" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);

   //// spend 2 from wlt to scrAddrF, rest back to scrAddrA ////
   auto spendVal = 2 * COIN;
   Signer signer;

   //instantiate resolver feed overloaded object
   auto feed = make_shared<ResolverUtils::TestResolverFeed>();

   auto addToFeed = [feed](const BinaryData& key)->void
   {
      auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
      feed->h160ToPubKey_.insert(datapair);
      feed->pubKeyToPrivKey_[datapair.second] = key;
   };

   addToFeed(TestChain::privKeyAddrB);
   addToFeed(TestChain::privKeyAddrC);
   addToFeed(TestChain::privKeyAddrD);
   addToFeed(TestChain::privKeyAddrE);

   //get utxo list for spend value
   auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

   //create script spender objects
   auto getSpenderPtr = [feed](
      const UnspentTxOut& utxo)->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   uint64_t total = 0;
   for (auto& utxo : unspentVec)
   {
      total += utxo.getValue();
      signer.addSpender(getSpenderPtr(utxo));
   }

   //add spend to addr F, use P2PKH
   auto recipientF = make_shared<Recipient_P2PKH>(
      TestChain::scrAddrF.getSliceCopy(1, 20), spendVal);
   signer.addRecipient(recipientF);

   if (total > spendVal)
   {
      //deal with change, no fee
      auto changeVal = total - spendVal;
      auto recipientA = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrA.getSliceCopy(1, 20), changeVal);
      signer.addRecipient(recipientA);
   }

   signer.sign();
   EXPECT_TRUE(signer.verify());
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_SizeEstimates)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a r value
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      5); //set lookup computation to 5 entries

   //register with db
   vector<BinaryData> addrVec;

   auto hashSet = assetWlt->getAddrHashSet();
   vector<BinaryData> hashVec;
   hashVec.insert(hashVec.begin(), hashSet.begin(), hashSet.end());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   for (auto& scripthash : hashSet)
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(scripthash);
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   }

   {
      ////spend 27 from wlt to assetWlt's first 2 unused addresses
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to first address
      auto addr0 = assetWlt->getNewAddress();
      signer.addRecipient(addr0->getRecipient(12 * COIN));
      addrVec.push_back(addr0->getPrefixedHash());

      //spend 15 to addr 1, use P2PKH
      auto addr1 = assetWlt->getNewAddress();
      signer.addRecipient(addr1->getRecipient(15 * COIN));
      addrVec.push_back(addr1->getPrefixedHash());

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto addr2 = assetWlt->getNewChangeAddress();
         signer.addRecipient(addr2->getRecipient(changeVal));
         addrVec.push_back(addr2->getPrefixedHash());
      }

      //add op_return output for coverage
      auto opreturn_msg = BinaryData::fromString("testing op_return");
      signer.addRecipient(make_shared<Recipient_OPRETURN>(opreturn_msg));

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 3 * COIN);

   uint64_t feeVal = 0;
   {
      ////spend 18 back to scrAddrB, with change to addr[2]

      auto spendVal = 18 * COIN;
      Signer signer2;

      auto getUtxos = [dbAssetWlt](uint64_t)->vector<UTXO>
      {
         auto&& unspentVec = dbAssetWlt->getSpendableTxOutListZC();

         vector<UTXO> utxoVec;
         for (auto& unspentTxo : unspentVec)
         {
            UTXO entry(unspentTxo.value_, unspentTxo.txHeight_, 
               unspentTxo.txIndex_, unspentTxo.txOutIndex_,
               move(unspentTxo.txHash_), move(unspentTxo.script_));

            utxoVec.emplace_back(entry);
         }

         return utxoVec;
      };

      auto&& addrBook = dbAssetWlt->createAddressBook();
      auto topBlock = theBDMt_->bdm()->blockchain()->top()->getBlockHeight();
      CoinSelectionInstance csi(assetWlt, getUtxos,
         addrBook, dbAssetWlt->getUnconfirmedBalance(topBlock), 
         topBlock);

      //spend 18 to addr B, use P2PKH
      csi.addRecipient(TestChain::scrAddrB, spendVal);

      float desiredFeeByte = 200.0f;
      csi.selectUTXOs(0, desiredFeeByte, 0);
      auto&& utxoSelect = csi.getUtxoSelection();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : utxoSelect)
      {
         total += utxo.getValue();
         signer2.addSpender(make_shared<ScriptSpender>(utxo, assetFeed));
      }

      //add recipients to signer
      auto& csRecipients = csi.getRecipients();
      for (auto& csRec : csRecipients)
         signer2.addRecipient(csRec.second);

      if (total > spendVal)
      {
         //deal with change
         auto changeVal = total - spendVal - csi.getFlatFee();
         feeVal = csi.getFlatFee();
         auto addr3 = assetWlt->getNewChangeAddress(
            AddressEntryType(AddressEntryType_P2WPKH | AddressEntryType_P2SH));
         signer2.addRecipient(addr3->getRecipient(changeVal));
         addrVec.push_back(addr3->getPrefixedHash());
      }

      //sign, verify & broadcast
      {
         auto&& lock = assetWlt->lockDecryptedContainer();
         signer2.sign();
      }

      EXPECT_TRUE(signer2.verify());

      DBTestUtils::ZcVector zcVec2;
      auto txref = signer2.serialize();

      //size estimate should not deviate from the signed tx size by more than 4 bytes
      //per input (DER sig size variance)
      EXPECT_TRUE(csi.getSizeEstimate() < txref.getSize() + utxoSelect.size() * 2);
      EXPECT_TRUE(csi.getSizeEstimate() > txref.getSize() - utxoSelect.size() * 2);

      zcVec2.push_back(signer2.serialize(), 15000000);

      //check fee/byte matches tx size
      auto totalFee = total - zcVec2.zcVec_[0].first.getSumOfOutputs();
      EXPECT_EQ(totalFee, csi.getFlatFee());
      float fee_byte = float(totalFee) / float(zcVec2.zcVec_[0].first.getTxWeight());
      auto fee_byte_diff = fee_byte - desiredFeeByte;

      EXPECT_TRUE(fee_byte_diff < 2.0f);
      EXPECT_TRUE(fee_byte_diff > -2.0f);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 3 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN - feeVal);

   uint64_t feeVal2;
   {
      ////spend 18 back to scrAddrB, with change to addr[2]

      Signer signer3;
      signer3.setFlags(SCRIPT_VERIFY_SEGWIT);

      auto getUtxos = [dbAssetWlt](uint64_t)->vector<UTXO>
      {
         auto&& unspentVec = dbAssetWlt->getSpendableTxOutListZC();

         vector<UTXO> utxoVec;
         for (auto& unspentTxo : unspentVec)
         {
            UTXO entry(unspentTxo.value_, unspentTxo.txHeight_,
               unspentTxo.txIndex_, unspentTxo.txOutIndex_,
               move(unspentTxo.txHash_), move(unspentTxo.script_));

            utxoVec.emplace_back(entry);
         }

         return utxoVec;
      };

      auto&& addrBook = dbAssetWlt->createAddressBook();
      auto topBlock = theBDMt_->bdm()->blockchain()->top()->getBlockHeight();
      CoinSelectionInstance csi(assetWlt, getUtxos,
         addrBook, dbAssetWlt->getUnconfirmedBalance(topBlock),
         topBlock);

      //have to add the recipient with 0 val for MAX fee estimate
      float desiredFeeByte = 200.0f;
      auto recipientID = csi.addRecipient(TestChain::scrAddrD, 0);
      feeVal2 = csi.getFeeForMaxVal(desiredFeeByte);
      auto spendVal = dbAssetWlt->getUnconfirmedBalance(topBlock);
      spendVal -= feeVal2;

      //spend 18 to addr D, use P2PKH
      csi.updateRecipient(recipientID, TestChain::scrAddrD, spendVal);

      csi.selectUTXOs(0, desiredFeeByte, 0);
      auto&& utxoSelect = csi.getUtxoSelection();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : utxoSelect)
      {
         total += utxo.getValue();
         signer3.addSpender(make_shared<ScriptSpender>(utxo, assetFeed));
      }

      //add recipients to signer
      auto& csRecipients = csi.getRecipients();
      for (auto& csRec : csRecipients)
         signer3.addRecipient(csRec.second);

      EXPECT_EQ(total, spendVal + feeVal2);

      //sign, verify & broadcast
      {
         auto&& lock = assetWlt->lockDecryptedContainer();
         signer3.sign();
      }

      EXPECT_TRUE(signer3.verify());

      DBTestUtils::ZcVector zcVec2;
      auto txref = signer3.serialize();

      //size estimate should not deviate from the signed tx size by more than 4 bytes
      //per input (DER sig size variance)
      EXPECT_TRUE(csi.getSizeEstimate() < txref.getSize() + utxoSelect.size() * 2);
      EXPECT_TRUE(csi.getSizeEstimate() > txref.getSize() - utxoSelect.size() * 2);

      zcVec2.push_back(signer3.serialize(), 15000000);

      //check fee/byte matches tx size
      auto totalFee = total - zcVec2.zcVec_[0].first.getSumOfOutputs();
      EXPECT_EQ(totalFee, csi.getFlatFee());
      float fee_byte = float(totalFee) / float(zcVec2.zcVec_[0].first.getTxWeight());
      auto fee_byte_diff = fee_byte - desiredFeeByte;

      EXPECT_TRUE(fee_byte_diff < 2.0f);
      EXPECT_TRUE(fee_byte_diff > -2.0f);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 17 * COIN - feeVal - feeVal2);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_P2WPKH)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      5); //set lookup computation to 3 entries

   //register with db
   vector<shared_ptr<AddressEntry>> addrVec;
   addrVec.push_back(assetWlt->getNewAddress(AddressEntryType_P2WPKH));
   addrVec.push_back(assetWlt->getNewAddress(AddressEntryType_P2WPKH));
   addrVec.push_back(assetWlt->getNewAddress(AddressEntryType_P2WPKH));

   vector<BinaryData> hashVec;
   for (auto addrPtr : addrVec)
      hashVec.push_back(addrPtr->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   for (auto& addrPtr : addrVec)
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(addrPtr->getPrefixedHash());
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   }

   {
      ////spend 27 from wlt to assetWlt's first 2 unused addresses
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to addr0, use P2WPKH
      signer.addRecipient(addrVec[0]->getRecipient(12 * COIN));

      //spend 15 to addr1, use P2WPKH
      signer.addRecipient(addrVec[1]->getRecipient(15 * COIN));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 18 back to scrAddrB, with change to addr2

      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListZC();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      //spend 18 to scrAddrB, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
      signer2.addRecipient(recipient2);

      if (total > spendVal)
      {
         //change to addr2, use P2WPKH
         auto changeVal = total - spendVal;
         auto addr2 = assetWlt->getNewAddress(AddressEntryType_P2WPKH);
         signer2.addRecipient(addrVec[2]->getRecipient(changeVal));
      }

      //sign, verify & broadcast
      {
         auto&& lock = assetWlt->lockDecryptedContainer();
         signer2.sign();
      }
      EXPECT_TRUE(signer2.verify());

      DBTestUtils::ZcVector zcVec2;
      zcVec2.push_back(signer2.serialize(), 15000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_MultipleSigners_1of3)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create 3 assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt_1 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   wltRoot = move(CryptoPRNG::generateRandom(32));
   auto assetWlt_2 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   wltRoot = move(CryptoPRNG::generateRandom(32));
   auto assetWlt_3 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   //create 1-of-3 multisig asset entry from 3 different wallets
   map<BinaryData, shared_ptr<AssetEntry>> asset_single_map;
   auto asset1 = assetWlt_1->getMainAccountAssetForIndex(0);
   auto wltid1_bd = assetWlt_1->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid1_bd), asset1));

   auto asset2 = assetWlt_2->getMainAccountAssetForIndex(0);
   auto wltid2_bd = assetWlt_2->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid2_bd), asset2));

   auto asset3 = assetWlt_3->getMainAccountAssetForIndex(0);
   auto wltid3_bd = assetWlt_3->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid3_bd), asset3));

   auto ae_ms = make_shared<AssetEntry_Multisig>(0, BinaryData::fromString("test"),
      asset_single_map, 1, 3);
   auto addr_ms_raw = make_shared<AddressEntry_Multisig>(ae_ms, true);
   auto addr_p2wsh = make_shared<AddressEntry_P2WSH>(addr_ms_raw);
   auto addr_ms = make_shared<AddressEntry_P2SH>(addr_p2wsh);

   //register with db
   vector<BinaryData> addrVec;
   addrVec.push_back(addr_ms->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, addrVec, "ms_entry");
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto ms_wlt = bdvPtr->getWalletOrLockbox("ms_entry");


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 27 from wlt to ms_wlt only address
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 27 nested p2wsh script hash
      signer.addRecipient(addr_ms->getRecipient(27 * COIN));

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //add op_return output for coverage
      auto opreturn_msg = BinaryData::fromString("testing op_return 0123");
      signer.addRecipient(make_shared<Recipient_OPRETURN>(opreturn_msg));

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);

   //lambda to sign with each wallet
   auto signPerWallet = [&](shared_ptr<AssetWallet_Single> wltPtr)->BinaryData
   {
      ////spend 18 back to scrAddrB, with change to self

      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec =
         ms_wlt->getSpendableTxOutListZC();

      //create feed from asset wallet
      auto feed = make_shared<ResolverFeed_AssetWalletSingle_ForMultisig>(wltPtr);
      auto assetFeed = make_shared<ResolverUtils::CustomFeed>(addr_ms, feed);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      //spend 18 to addr 0, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
      signer2.addRecipient(recipient2);

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         signer2.addRecipient(addr_ms->getRecipient(changeVal));
      }

      //add op_return output for coverage
      auto opreturn_msg = BinaryData::fromString("testing op_return 0123");
      signer2.addRecipient(make_shared<Recipient_OPRETURN>(opreturn_msg));

      //sign, verify & return signed tx
      {
         auto lock = wltPtr->lockDecryptedContainer();
         signer2.sign();
      }
      EXPECT_TRUE(signer2.verify());

      return signer2.serialize();
   };

   //call lambda with each wallet
   auto&& tx1 = signPerWallet(assetWlt_1);
   auto&& tx2 = signPerWallet(assetWlt_2);
   auto&& tx3 = signPerWallet(assetWlt_3);

   //broadcast the last one
   DBTestUtils::ZcVector zcVec;
   zcVec.push_back(tx3, 15000000);

   DBTestUtils::pushNewZc(theBDMt_, zcVec);
   DBTestUtils::waitOnNewZcSignal(clients_, bdvID);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_MultipleSigners_2of3_NativeP2WSH)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create 3 assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt_1 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   wltRoot = move(CryptoPRNG::generateRandom(32));
   auto assetWlt_2 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   wltRoot = move(CryptoPRNG::generateRandom(32));
   auto assetWlt_3 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   //create 2-of-3 multisig asset entry from 3 different wallets
   map<BinaryData, shared_ptr<AssetEntry>> asset_single_map;
   auto asset1 = assetWlt_1->getMainAccountAssetForIndex(0);
   auto wltid1_bd = assetWlt_1->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid1_bd), asset1));

   auto asset2 = assetWlt_2->getMainAccountAssetForIndex(0);
   auto wltid2_bd = assetWlt_2->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid2_bd), asset2));

   auto asset4_singlesig = assetWlt_2->getNewAddress();

   auto asset3 = assetWlt_3->getMainAccountAssetForIndex(0);
   auto wltid3_bd = assetWlt_3->getID();
   asset_single_map.insert(make_pair(BinaryData::fromString(wltid3_bd), asset3));

   auto ae_ms = make_shared<AssetEntry_Multisig>(0, BinaryData::fromString("test"),
      asset_single_map, 2, 3);
   auto addr_ms_raw = make_shared<AddressEntry_Multisig>(ae_ms, true);
   auto addr_p2wsh = make_shared<AddressEntry_P2WSH>(addr_ms_raw);


   //register with db
   vector<BinaryData> addrVec;
   addrVec.push_back(addr_p2wsh->getPrefixedHash());

   vector<BinaryData> addrVec_singleSig;
   auto&& addrSet = assetWlt_2->getAddrHashSet();
   for (auto& addr : addrSet)
      addrVec_singleSig.push_back(addr);

   DBTestUtils::registerWallet(clients_, bdvID, addrVec, "ms_entry");
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   DBTestUtils::registerWallet(clients_, bdvID, addrVec_singleSig, assetWlt_2->getID());

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto ms_wlt = bdvPtr->getWalletOrLockbox("ms_entry");
   auto wlt_singleSig = bdvPtr->getWalletOrLockbox(assetWlt_2->getID());


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 27 from wlt to ms_wlt only address
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 20 to nested p2wsh script hash
      signer.addRecipient(addr_p2wsh->getRecipient(20 * COIN));

      //spend 7 to assetWlt_2
      signer.addRecipient(asset4_singlesig->getRecipient(7 * COIN));

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());
      auto&& zcHash = signer.getTxId();

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);

	   //grab ZC from DB and verify it again
      auto&& zc_from_db = DBTestUtils::getTxByHash(clients_, bdvID, zcHash);
      auto&& raw_tx = zc_from_db.serialize();
      auto bctx = BCTX::parse(raw_tx);
      TransactionVerifier tx_verifier(*bctx, utxoVec);

      ASSERT_TRUE(tx_verifier.evaluateState().isValid());
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 20 * COIN);
   scrObj = wlt_singleSig->getScrAddrObjByKey(asset4_singlesig->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 7 * COIN);

   auto spendVal = 18 * COIN;
   Signer signer2;
   signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

   //get utxo list for spend value
   auto&& unspentVec =
      ms_wlt->getSpendableTxOutListZC();

   auto&& unspentVec_singleSig = wlt_singleSig->getSpendableTxOutListZC();

   unspentVec.insert(unspentVec.end(),
      unspentVec_singleSig.begin(), unspentVec_singleSig.end());

   //create feed from asset wallet 1
   auto feed_ms = make_shared<ResolverFeed_AssetWalletSingle_ForMultisig>(assetWlt_1);
   auto assetFeed = make_shared<ResolverUtils::CustomFeed>(addr_p2wsh, feed_ms);

   //create spenders
   uint64_t total = 0;
   for (auto& utxo : unspentVec)
   {
      total += utxo.getValue();
      signer2.addSpender(getSpenderPtr(utxo, assetFeed));
   }

   //creates outputs
   //spend 18 to addr 0, use P2PKH
   auto recipient2 = make_shared<Recipient_P2PKH>(
      TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
   signer2.addRecipient(recipient2);

   if (total > spendVal)
   {
      //deal with change, no fee
      auto changeVal = total - spendVal;
      signer2.addRecipient(addr_p2wsh->getRecipient(changeVal));
   }

   //sign, verify & return signed tx
   auto&& signerState = signer2.evaluateSignedState();

   {
      EXPECT_EQ(signerState.getEvalMapSize(), 2);

      auto&& txinEval = signerState.getSignedStateForInput(0);
      auto& pubkeyMap = txinEval.getPubKeyMap();
      EXPECT_EQ(pubkeyMap.size(), 3);
      for (auto& pubkeyState : pubkeyMap)
         EXPECT_FALSE(pubkeyState.second);

      txinEval = signerState.getSignedStateForInput(1);
      auto& pubkeyMap_2 = txinEval.getPubKeyMap();
      EXPECT_EQ(pubkeyMap_2.size(), 0);
   }

   {
      auto lock = assetWlt_1->lockDecryptedContainer();
      signer2.sign();
   }

   try
   {
      signer2.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   {
      //signer state with 1 sig
      EXPECT_FALSE(signer2.isValid());
      signerState = signer2.evaluateSignedState();

      EXPECT_EQ(signerState.getEvalMapSize(), 2);

      auto&& txinEval = signerState.getSignedStateForInput(0);
      EXPECT_EQ(txinEval.getSigCount(), 1);

      auto asset_single = dynamic_pointer_cast<AssetEntry_Single>(asset1);
      ASSERT_NE(asset_single, nullptr);
      ASSERT_TRUE(txinEval.isSignedForPubKey(asset_single->getPubKey()->getCompressedKey()));
   }

   Signer signer3;
   //create feed from asset wallet 2
   auto feed_ms3 = make_shared<ResolverFeed_AssetWalletSingle_ForMultisig>(assetWlt_2);
   auto assetFeed3 = make_shared<ResolverUtils::CustomFeed>(addr_p2wsh, feed_ms3);
   signer3.deserializeState(signer2.serializeState());

   {
      //make sure sig was properly carried over with state
      EXPECT_FALSE(signer3.isValid());
      signerState = signer3.evaluateSignedState();

      EXPECT_EQ(signerState.getEvalMapSize(), 2);
      auto&& txinEval = signerState.getSignedStateForInput(0);
      EXPECT_EQ(txinEval.getSigCount(), 1);

      auto asset_single = dynamic_pointer_cast<AssetEntry_Single>(asset1);
      ASSERT_NE(asset_single, nullptr);
      ASSERT_TRUE(txinEval.isSignedForPubKey(asset_single->getPubKey()->getCompressedKey()));
   }

   signer3.setFeed(assetFeed3);

   {
      auto lock = assetWlt_2->lockDecryptedContainer();
      signer3.sign();
   }

   {
      auto assetFeed4 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_2);
      signer3.resetFeeds();
      signer3.setFeed(assetFeed4);
      auto lock = assetWlt_2->lockDecryptedContainer();
      signer3.sign();
   }


   ASSERT_TRUE(signer3.isValid());
   try
   {
      signer3.verify();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   {
      //should have 2 sigs now
      EXPECT_TRUE(signer3.isValid());
      signerState = signer3.evaluateSignedState();

      EXPECT_EQ(signerState.getEvalMapSize(), 2);
      auto&& txinEval = signerState.getSignedStateForInput(0);
      EXPECT_EQ(txinEval.getSigCount(), 2);

      auto asset_single = dynamic_pointer_cast<AssetEntry_Single>(asset1);
      ASSERT_NE(asset_single, nullptr);
      ASSERT_TRUE(txinEval.isSignedForPubKey(asset_single->getPubKey()->getCompressedKey()));

      asset_single = dynamic_pointer_cast<AssetEntry_Single>(asset2);
      ASSERT_NE(asset_single, nullptr);
      ASSERT_TRUE(txinEval.isSignedForPubKey(asset_single->getPubKey()->getCompressedKey()));
   }

   auto&& tx1 = signer3.serialize();
   auto&& zcHash = signer3.getTxId();

   //broadcast the last one
   DBTestUtils::ZcVector zcVec;
   zcVec.push_back(tx1, 15000000);

   DBTestUtils::pushNewZc(theBDMt_, zcVec);
   DBTestUtils::waitOnNewZcSignal(clients_, bdvID);

   //grab ZC from DB and verify it again
   auto&& zc_from_db = DBTestUtils::getTxByHash(clients_, bdvID, zcHash);
   auto&& raw_tx = zc_from_db.serialize();
   auto bctx = BCTX::parse(raw_tx);
   TransactionVerifier tx_verifier(*bctx, unspentVec);

   ASSERT_TRUE(tx_verifier.evaluateState().isValid());


   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = ms_wlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);
   scrObj = wlt_singleSig->getScrAddrObjByKey(asset4_singlesig->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_MultipleSigners_DifferentInputs)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create 2 assetWlt ////

   //create a root private key
   auto assetWlt_1 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      CryptoPRNG::generateRandom(32), //root as rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   auto assetWlt_2 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(CryptoPRNG::generateRandom(32)), //root as rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   //register with db
   vector<shared_ptr<AddressEntry>> addrVec_1;
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());

   vector<BinaryData> hashVec_1;
   for (auto addrPtr : addrVec_1)
      hashVec_1.push_back(addrPtr->getPrefixedHash());

   vector<shared_ptr<AddressEntry>> addrVec_2;
   addrVec_2.push_back(assetWlt_2->getNewAddress());
   addrVec_2.push_back(assetWlt_2->getNewAddress());
   addrVec_2.push_back(assetWlt_2->getNewAddress());

   vector<BinaryData> hashVec_2;
   for (auto addrPtr : addrVec_2)
      hashVec_2.push_back(addrPtr->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_1, assetWlt_1->getID());
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_2, assetWlt_2->getID());

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto wlt_1 = bdvPtr->getWalletOrLockbox(assetWlt_1->getID());
   auto wlt_2 = bdvPtr->getWalletOrLockbox(assetWlt_2->getID());

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 12 to wlt_1, 15 to wlt_2 from wlt
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to p2pkh script hash
      signer.addRecipient(addrVec_1[0]->getRecipient(12 * COIN));

      //spend 15 to p2pkh script hash
      signer.addRecipient(addrVec_2[0]->getRecipient(15 * COIN));

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);

   //spend 18 back to wlt, split change among the 2

   //get utxo list for spend value
   auto&& unspentVec_1 =
      wlt_1->getSpendableTxOutListZC();
   auto&& unspentVec_2 =
      wlt_2->getSpendableTxOutListZC();

   BinaryData serializedSignerState;

   auto assetFeed2 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_1);
   auto assetFeed3 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_2);

   {
      auto spendVal = 8 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //create feed from asset wallet 1

      //create wlt_1 spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec_1)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed2));
      }

      //spend 18 to addrB, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), 18 * COIN);
      signer2.addRecipient(recipient2);

      //change back to wlt_1
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer2.addRecipient(addrVec_1[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer2.serializeState());
   }

   {
      //serialize signer 2, deser with signer3 and populate
      auto spendVal = 10 * COIN;
      Signer signer3;
      signer3.deserializeState(serializedSignerState);

      //add spender from wlt_2
      uint64_t total = 0;
      for (auto& utxo : unspentVec_2)
      {
         total += utxo.getValue();
         signer3.addSpender(getSpenderPtr(utxo, assetFeed3));
      }

      //set change
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer3.addRecipient(addrVec_2[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer3.serializeState());
   }


   //sign, verify & return signed tx
   Signer signer4;
   signer4.deserializeState(serializedSignerState);
   signer4.setFeed(assetFeed2);

   {
      auto lock = assetWlt_1->lockDecryptedContainer();
      signer4.sign();
   }

   try
   {
      signer4.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   EXPECT_FALSE(signer4.isValid());

   Signer signer5;
   signer5.deserializeState(signer4.serializeState());
   signer5.setFeed(assetFeed3);

   {
      auto lock = assetWlt_2->lockDecryptedContainer();
      signer5.sign();
   }

   ASSERT_TRUE(signer5.isValid());
   try
   {
      signer5.verify();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   auto&& tx1 = signer5.serialize();

   //broadcast the last one
   DBTestUtils::ZcVector zcVec;
   zcVec.push_back(tx1, 15000000);

   DBTestUtils::pushNewZc(theBDMt_, zcVec);
   DBTestUtils::waitOnNewZcSignal(clients_, bdvID);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 4 * COIN);

   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_MultipleSigners_ParallelSigning)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create 2 assetWlt ////

   //create a root private key
   auto assetWlt_1 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      CryptoPRNG::generateRandom(32), //root as rvalue
      {},
      SecureBinaryData(), //empty passphrase
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   auto assetWlt_2 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(CryptoPRNG::generateRandom(32)), //root as rvalue
      {},
      SecureBinaryData(), //empty passphrase
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   //register with db
   vector<shared_ptr<AddressEntry>> addrVec_1;
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());

   vector<BinaryData> hashVec_1;
   for (auto addrPtr : addrVec_1)
      hashVec_1.push_back(addrPtr->getPrefixedHash());

   vector<shared_ptr<AddressEntry>> addrVec_2;
   addrVec_2.push_back(assetWlt_2->getNewAddress());
   addrVec_2.push_back(assetWlt_2->getNewAddress());
   addrVec_2.push_back(assetWlt_2->getNewAddress());

   vector<BinaryData> hashVec_2;
   for (auto addrPtr : addrVec_2)
      hashVec_2.push_back(addrPtr->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_1, assetWlt_1->getID());
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_2, assetWlt_2->getID());

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto wlt_1 = bdvPtr->getWalletOrLockbox(assetWlt_1->getID());
   auto wlt_2 = bdvPtr->getWalletOrLockbox(assetWlt_2->getID());

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 12 to wlt_1, 15 to wlt_2 from wlt
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to p2pkh script hash
      signer.addRecipient(addrVec_1[0]->getRecipient(12 * COIN));

      //spend 15 to p2pkh script hash
      signer.addRecipient(addrVec_2[0]->getRecipient(15 * COIN));

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);

   //spend 18 back to wlt, split change among the 2

   //get utxo list for spend value
   auto&& unspentVec_1 =
      wlt_1->getSpendableTxOutListZC();
   auto&& unspentVec_2 =
      wlt_2->getSpendableTxOutListZC();

   BinaryData serializedSignerState;

   {
      //create first signer, set outpoint from wlt_1 and change to wlt_1
      auto spendVal = 8 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //create feed from asset wallet 1

      //create wlt_1 spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec_1)
      {
         total += utxo.getValue();
         signer2.addSpender(
            make_shared<ScriptSpender>(
            utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue()));
      }

      //spend 18 to addrB, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), 18 * COIN);
      signer2.addRecipient(recipient2);

      //change back to wlt_1
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer2.addRecipient(addrVec_1[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer2.serializeState());
   }

   {
      //serialize signer 2, deser with signer3 and populate with outpoint and 
      //change from wlt_2
      auto spendVal = 10 * COIN;
      Signer signer3;
      signer3.deserializeState(serializedSignerState);

      //add spender from wlt_2
      uint64_t total = 0;
      for (auto& utxo : unspentVec_2)
      {
         total += utxo.getValue();
         signer3.addSpender(
            make_shared<ScriptSpender>(
            utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue()));
      }

      //set change
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer3.addRecipient(addrVec_2[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer3.serializeState());
   }

   auto assetFeed2 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_1);
   auto assetFeed3 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_2);

   //deser to new signer, this time populate with feed and utxo from wlt_1
   Signer signer4;
   for (auto& utxo : unspentVec_1)
   {
      signer4.addSpender(getSpenderPtr(utxo, assetFeed2));
   }

   signer4.deserializeState(serializedSignerState);

   {
      auto lock = assetWlt_1->lockDecryptedContainer();
      signer4.sign();
   }

   try
   {
      signer4.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   EXPECT_FALSE(signer4.isValid());

   //deser from same state into wlt_2 signer
   Signer signer5;

   //in this case, we can't set the utxos first then deser the state, as it would break
   //utxo ordering. we have to deser first, then populate utxos
   signer5.deserializeState(serializedSignerState);

   for (auto& utxo : unspentVec_2)
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      signer5.populateUtxo(entry);
   }

   //finally set the feed
   signer5.setFeed(assetFeed3);

   {
      auto lock = assetWlt_2->lockDecryptedContainer();
      signer5.sign();
   }

   try
   {
      signer5.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   //now serialize both signers into the final signer, verify and broadcast
   Signer signer6;
   signer6.deserializeState(signer4.serializeState());
   signer6.deserializeState(signer5.serializeState());

   ASSERT_TRUE(signer6.isValid());
   try
   {
      signer6.verify();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   //try again in the opposite order, that should not matter
   Signer signer7;
   signer7.deserializeState(signer5.serializeState());
   signer7.deserializeState(signer4.serializeState());

   ASSERT_TRUE(signer7.isValid());
   try
   {
      signer7.verify();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   auto&& tx1 = signer7.serialize();

   //broadcast the last one
   DBTestUtils::ZcVector zcVec;
   zcVec.push_back(tx1, 15000000);

   DBTestUtils::pushNewZc(theBDMt_, zcVec);
   DBTestUtils::waitOnNewZcSignal(clients_, bdvID);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 4 * COIN);

   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, GetUnsignedTxId)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create 2 assetWlt ////

   //create a root private key
   auto assetWlt_1 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      CryptoPRNG::generateRandom(32), //root as rvalue
      {},
      SecureBinaryData(), //empty passphrase
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   auto assetWlt_2 = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(CryptoPRNG::generateRandom(32)), //root as rvalue
      {},
      SecureBinaryData(), //empty passphrase
      SecureBinaryData(),
      3); //set lookup computation to 3 entries

   //register with db
   vector<shared_ptr<AddressEntry>> addrVec_1;
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());
   addrVec_1.push_back(assetWlt_1->getNewAddress());

   vector<BinaryData> hashVec_1;
   for (auto addrPtr : addrVec_1)
      hashVec_1.push_back(addrPtr->getPrefixedHash());

   vector<shared_ptr<AddressEntry>> addrVec_2;
   auto addr_type_nested_p2wsh = AddressEntryType(AddressEntryType_P2WPKH | AddressEntryType_P2SH);
   addrVec_2.push_back(assetWlt_2->getNewAddress(addr_type_nested_p2wsh));
   addrVec_2.push_back(assetWlt_2->getNewAddress(addr_type_nested_p2wsh));
   addrVec_2.push_back(assetWlt_2->getNewAddress(addr_type_nested_p2wsh));

   vector<BinaryData> hashVec_2;
   for (auto addrPtr : addrVec_2)
      hashVec_2.push_back(addrPtr->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_1, assetWlt_1->getID());
   DBTestUtils::registerWallet(clients_, bdvID, hashVec_2, assetWlt_2->getID());

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto wlt_1 = bdvPtr->getWalletOrLockbox(assetWlt_1->getID());
   auto wlt_2 = bdvPtr->getWalletOrLockbox(assetWlt_2->getID());

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 12 to wlt_1, 15 to wlt_2 from wlt
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to p2pkh script hash
      signer.addRecipient(addrVec_1[0]->getRecipient(12 * COIN));

      //spend 15 to p2pkh script hash
      signer.addRecipient(addrVec_2[0]->getRecipient(15 * COIN));

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      try
      {
         //shouldn't be able to get txid on legacy unsigned tx
         signer.getTxId();
         EXPECT_TRUE(false);
      }
      catch (exception&)
      {
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = wlt_1->getScrAddrObjByKey(hashVec_1[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = wlt_2->getScrAddrObjByKey(hashVec_2[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);


   auto&& unspentVec_1 =
      wlt_1->getSpendableTxOutListZC();
   auto&& unspentVec_2 =
      wlt_2->getSpendableTxOutListZC();

   BinaryData serializedSignerState;

   {
      //create first signer, set outpoint from wlt_1 and change to wlt_1
      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //create feed from asset wallet 1

      //create wlt_1 spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec_1)
      {
         total += utxo.getValue();
         signer2.addSpender(
            make_shared<ScriptSpender>(
            utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue()));
      }

      //spend 18 to addrB, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), 18 * COIN);
      signer2.addRecipient(recipient2);

      //change back to wlt_1
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer2.addRecipient(addrVec_1[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer2.serializeState());
   }

   {
      //serialize signer 2, deser with signer3 and populate with outpoint and 
      //change from wlt_2
      auto spendVal = 10 * COIN;
      Signer signer3;
      signer3.deserializeState(serializedSignerState);

      //add spender from wlt_2
      uint64_t total = 0;
      for (auto& utxo : unspentVec_2)
      {
         total += utxo.getValue();
         signer3.addSpender(
            make_shared<ScriptSpender>(
            utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue()));
      }

      //set change
      if (total > spendVal)
      {
         //spend 4 to p2pkh script hash
         signer3.addRecipient(addrVec_2[1]->getRecipient(total - spendVal));
      }

      serializedSignerState = move(signer3.serializeState());
   }

   auto assetFeed2 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_1);
   auto assetFeed3 = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt_2);

   //deser to new signer, this time populate with feed and utxo from wlt_1
   Signer signer4;
   for (auto& utxo : unspentVec_1)
   {
      signer4.addSpender(getSpenderPtr(utxo, assetFeed2));
   }

   signer4.deserializeState(serializedSignerState);

   {
      auto lock = assetWlt_1->lockDecryptedContainer();
      signer4.sign();
   }

   try
   {
      signer4.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   EXPECT_FALSE(signer4.isValid());

   //should fail to get txid
   try
   {
      signer4.getTxId();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   //deser from same state into wlt_2 signer
   Signer signer5;

   //in this case, we can't set the utxos first then deser the state, as it would break
   //utxo ordering. we have to deser first, then populate utxos
   signer5.deserializeState(signer4.serializeState());

   //should fail since we lack the utxos
   try
   {
      signer5.getTxId();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   for (auto& utxo : unspentVec_2)
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      signer5.populateUtxo(entry);
   }

   //finally set the feed
   signer5.setFeed(assetFeed3);

   //tx should be unsigned
   try
   {
      signer5.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   //should produce valid txid without signing
   BinaryData txid;
   try
   {
      txid = signer5.getTxId();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   //producing a txid should not change the signer status from unsigned to signed
   try
   {
      signer5.verify();
      EXPECT_TRUE(false);
   }
   catch (...)
   {
   }

   {
      auto lock = assetWlt_2->lockDecryptedContainer();
      signer5.sign();
   }

   try
   {
      signer5.verify();
   }
   catch (...)
   {
      EXPECT_TRUE(false);
   }

   //check txid pre sig with txid post sig
   EXPECT_EQ(txid, signer5.getTxId());
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, Wallet_SpendTest_Nested_P2WPKH)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a r value
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //lookup computation

   //register with db
   vector<BinaryData> addrVec;

   auto hashSet = assetWlt->getAddrHashSet();
   vector<BinaryData> hashVec;
   hashVec.insert(hashVec.begin(), hashSet.begin(), hashSet.end());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   for (auto& scripthash : hashSet)
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(scripthash);
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   }

   {
      ////spend 27 from wlt to assetWlt's first 2 unused addresses
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to addr0, nested P2WPKH
      auto addr0 = assetWlt->getNewAddress(
         AddressEntryType(AddressEntryType_P2WPKH | AddressEntryType_P2SH));
      signer.addRecipient(addr0->getRecipient(12 * COIN));
      addrVec.push_back(addr0->getPrefixedHash());

      //spend 15 to addr1, nested P2WPKH
      auto addr1 = assetWlt->getNewAddress(
         AddressEntryType(AddressEntryType_P2WPKH | AddressEntryType_P2SH));
      signer.addRecipient(addr1->getRecipient(15 * COIN));
      addrVec.push_back(addr1->getPrefixedHash());

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);

   {
      ////spend 18 back to scrAddrB, with change to addr[2]

      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec =
         dbAssetWlt->getSpendableTxOutListZC();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      //spend 18 to addr 0, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
      signer2.addRecipient(recipient2);

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto addr2 = assetWlt->getNewAddress(
            AddressEntryType(AddressEntryType_P2WPKH | AddressEntryType_P2SH));
         signer2.addRecipient(addr2->getRecipient(changeVal));
         addrVec.push_back(addr2->getPrefixedHash());
      }

      //sign, verify & broadcast
      {
         auto lock = assetWlt->lockDecryptedContainer();
         signer2.sign();
      }

      EXPECT_TRUE(signer2.verify());

      DBTestUtils::ZcVector zcVec2;
      zcVec2.push_back(signer2.serialize(), 15000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, Wallet_SpendTest_Nested_P2PK)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a r value
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      3); //lookup computation

   //register with db
   vector<BinaryData> addrVec;

   auto hashSet = assetWlt->getAddrHashSet();
   vector<BinaryData> hashVec;
   hashVec.insert(hashVec.begin(), hashSet.begin(), hashSet.end());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());


   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   for (auto& scripthash : hashSet)
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(scripthash);
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   }

   {
      ////spend 27 from wlt to assetWlt's first 2 unused addresses
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to addr0, nested P2WPKH
      auto addr0 = assetWlt->getNewAddress(
         AddressEntryType(
         AddressEntryType_P2PK | AddressEntryType_P2SH | AddressEntryType_Compressed));
      signer.addRecipient(addr0->getRecipient(12 * COIN));
      addrVec.push_back(addr0->getPrefixedHash());

      //spend 15 to addr1, nested P2WPKH
      auto addr1 = assetWlt->getNewAddress(
         AddressEntryType(
         AddressEntryType_P2PK | AddressEntryType_P2SH | AddressEntryType_Compressed));
      signer.addRecipient(addr1->getRecipient(15 * COIN));
      addrVec.push_back(addr1->getPrefixedHash());

      if (total > spendVal)
      {
         //change to scrAddrD, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);

   {
      ////spend 18 back to scrAddrB, with change to addr[2]

      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec =
         dbAssetWlt->getSpendableTxOutListZC();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      //spend 18 to addr 0, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
      signer2.addRecipient(recipient2);

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto addr2 = assetWlt->getNewAddress(
            AddressEntryType(
            AddressEntryType_P2PK | AddressEntryType_P2SH | AddressEntryType_Compressed));
         signer2.addRecipient(addr2->getRecipient(changeVal));
         addrVec.push_back(addr2->getPrefixedHash());
      }

      //add opreturn for coverage
      auto opreturn_msg = BinaryData::fromString("op_return message testing");
      signer2.addRecipient(make_shared<Recipient_OPRETURN>(opreturn_msg));

      //sign, verify & broadcast
      {
         auto lock = assetWlt->lockDecryptedContainer();
         signer2.sign();
      }
      EXPECT_TRUE(signer2.verify());

      DBTestUtils::ZcVector zcVec2;
      zcVec2.push_back(signer2.serialize(), 15000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_FromAccount_Reload)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      SecureBinaryData(),
      SecureBinaryData(),
      5); //set lookup computation to 5 entries

   //register with db
   vector<shared_ptr<AddressEntry>> addrVec;
   auto accID = assetWlt->getMainAccountID();
   {
      auto accPtr = assetWlt->getAccountForID(accID);
      addrVec.push_back(accPtr->getNewAddress(AddressEntryType_P2WPKH));
      addrVec.push_back(accPtr->getNewAddress(AddressEntryType_P2WPKH));
      addrVec.push_back(accPtr->getNewAddress(AddressEntryType_P2WPKH));
   }

   vector<BinaryData> hashVec;
   for (auto addrPtr : addrVec)
      hashVec.push_back(addrPtr->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");

   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //check new wallet balances
   for (auto& addrPtr : addrVec)
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(addrPtr->getPrefixedHash());
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   }

   {
      ////spend 27 from wlt to assetWlt's first 2 unused addresses
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend 12 to addr0, use P2WPKH
      signer.addRecipient(addrVec[0]->getRecipient(12 * COIN));

      //spend 15 to addr1, use P2WPKH
      signer.addRecipient(addrVec[1]->getRecipient(15 * COIN));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //destroy wallet object
   auto fName = assetWlt->getDbFilename();
   ASSERT_EQ(assetWlt.use_count(), 1);
   assetWlt.reset();

   //reload it
   auto controlPassLbd = [](const set<BinaryData>&)->SecureBinaryData
   {
      return SecureBinaryData();
   };
   auto loadedWlt = AssetWallet::loadMainWalletFromFile(
      fName, controlPassLbd);
   assetWlt = dynamic_pointer_cast<AssetWallet_Single>(loadedWlt);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 12 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 15 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   {
      ////spend 18 back to scrAddrB, with change to addr2

      auto spendVal = 18 * COIN;
      Signer signer2;
      signer2.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListZC();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer2.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      //spend 18 to scrAddrB, use P2PKH
      auto recipient2 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrB.getSliceCopy(1, 20), spendVal);
      signer2.addRecipient(recipient2);

      if (total > spendVal)
      {
         //change to new address, use P2SH-P2WPKH
         auto accPtr = assetWlt->getAccountForID(accID);

         auto changeVal = total - spendVal;
         auto addr3 = accPtr->getNewAddress(
            AddressEntryType(AddressEntryType_P2SH | AddressEntryType_P2WPKH));
         signer2.addRecipient(addr3->getRecipient(changeVal));

         addrVec.push_back(addr3);
         hashVec.push_back(addr3->getPrefixedHash());
      }

      //sign, verify & broadcast
      {
         auto&& lock = assetWlt->lockDecryptedContainer();
         signer2.sign();
      }
      EXPECT_TRUE(signer2.verify());

      DBTestUtils::ZcVector zcVec2;
      zcVec2.push_back(signer2.serialize(), 15000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec2);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   try
   {
      scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]->getPrefixedHash());
      EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
      ASSERT_TRUE(false); //should never get here
   }
   catch (exception&)
   {}

   //register new change address
   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());

   //check new wallet balance again, change value should appear
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //full node cannot track zc prior to address registration, balance will
   //show after the zc mines
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //mine 2 blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //change balance will now show on post zc registered address
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);

   {
      //check there are no zc utxos anymore
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListZC();
      ASSERT_EQ(unspentVec.size(), 0);
   }

   {
      ////clean up change address

      auto spendVal = 9 * COIN;
      Signer signer3;
      signer3.setFlags(SCRIPT_VERIFY_SEGWIT);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue();

      //create feed from asset wallet
      auto assetFeed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //create spenders
      uint64_t total = 0;
      for (auto& utxo : unspentVec)
      {
         total += utxo.getValue();
         signer3.addSpender(getSpenderPtr(utxo, assetFeed));
      }

      //creates outputs
      auto recipient3 = make_shared<Recipient_P2PKH>(
         TestChain::scrAddrE.getSliceCopy(1, 20), spendVal);
      signer3.addRecipient(recipient3);

      EXPECT_EQ(total, spendVal);

      //sign, verify & broadcast
      {
         auto&& lock = assetWlt->lockDecryptedContainer();
         signer3.sign();
      }
      EXPECT_TRUE(signer3.verify());

      DBTestUtils::ZcVector zcVec3;
      zcVec3.push_back(signer3.serialize(), 15000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec3);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 48 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 9 * COIN);

   //check new wallet balances
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[0]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[1]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[2]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(addrVec[3]->getPrefixedHash());
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_BIP32_Accounts)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto passphrase = SecureBinaryData::fromString("test");
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromSeed_BIP32_Blank(
      homedir_,
      wltRoot, //root as a rvalue
      SecureBinaryData(),
      passphrase);

   //salted account
   auto&& salt = CryptoPRNG::generateRandom(32);
   auto saltedAccType =
      make_shared<AccountType_BIP32_Salted>(salt);
   saltedAccType->setAddressLookup(5);
   saltedAccType->setDefaultAddressType(
      AddressEntryType(AddressEntryType_P2SH | AddressEntryType_P2WPKH));
   saltedAccType->setAddressTypes(
      { AddressEntryType(AddressEntryType_P2SH | AddressEntryType_P2WPKH) });

   auto passphraseLbd = [&passphrase](const set<BinaryData>&)->SecureBinaryData
   {
      return passphrase;
   };
   assetWlt->setPassphrasePromptLambda(passphraseLbd);

   auto accountID1 = assetWlt->createBIP32Account(
      nullptr, { 0x80000099, 0x80000001 }, saltedAccType);

   //regular account
   auto mainAccType =
      make_shared<AccountType_BIP32_Custom>();
   mainAccType->setAddressLookup(5);
   mainAccType->setDefaultAddressType(
      AddressEntryType_P2WPKH);
   mainAccType->setAddressTypes(
      { AddressEntryType_P2WPKH });

   auto accountID2 = assetWlt->createBIP32Account(
      nullptr, { 0x80000099, 0x80000000 }, mainAccType);

   assetWlt->resetPassphrasePromptLambda();

   //register with db
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   auto accPtr1 = assetWlt->getAccountForID(accountID1);
   auto accPtr2 = assetWlt->getAccountForID(accountID2);

   auto newAddr1 = accPtr1->getNewAddress();
   auto newAddr2 = accPtr2->getNewAddress();
   auto newAddr3 = accPtr2->getNewAddress();

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   {
      ////spend 27 from wlt to acc1 & acc2
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr1->getRecipient(14 * COIN));
      signer.addRecipient(newAddr2->getRecipient(13 * COIN));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //register new wallet
   vector<BinaryData> hashVec;
   hashVec.push_back(newAddr1->getPrefixedHash());
   hashVec.push_back(newAddr2->getPrefixedHash());
   hashVec.push_back(newAddr3->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());

   //mine some blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 14 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 13 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);

   //spend from the new addresses
   {

      auto spendVal = 27 * COIN;
      Signer signer;

      auto feed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr3->getRecipient(spendVal));

      //sign, verify then broadcast
      {
         auto passlbd = [passphrase](const set<BinaryData>&)->SecureBinaryData
         {
            return passphrase;
         };

         assetWlt->setPassphrasePromptLambda(passlbd);
         auto lock = assetWlt->lockDecryptedContainer();
         signer.sign();
      }

      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[2]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_FromExtendedAddress_Armory135)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto passphrase = SecureBinaryData::fromString("test");
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromPrivateRoot_Armory135(
      homedir_,
      move(wltRoot), //root as a rvalue
      {},
      passphrase,
      SecureBinaryData::fromString("control"),
      5); //set lookup computation to 5 entries

   //register with db
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //grab enough addresses to trigger a lookup extention
   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 5);

   for (unsigned i = 0; i < 15; i++)
      assetWlt->getNewAddress();
   auto newAddr = assetWlt->getNewAddress();

   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 105);

   {
      ////spend 27 from wlt to newAddr
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr->getRecipient(spendVal));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //register new wallet
   vector<BinaryData> hashVec;
   hashVec.push_back(newAddr->getPrefixedHash());
   auto newAddr2 = assetWlt->getNewAddress();
   hashVec.push_back(newAddr2->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());

   //mine some blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);

   //spend from the new address
   {
      //spend from the new address

      auto spendVal = 27 * COIN;
      Signer signer;

      auto feed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr2->getRecipient(spendVal));

      //sign, verify then broadcast
      {
         auto passlbd = [passphrase](const set<BinaryData>&)->SecureBinaryData
         {
            return passphrase;
         };

         assetWlt->setPassphrasePromptLambda(passlbd);
         auto lock = assetWlt->lockDecryptedContainer();
         signer.sign();
      }

      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_FromExtendedAddress_BIP32)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto passphrase = SecureBinaryData::fromString("test");
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromSeed_BIP32(
      homedir_,
      move(wltRoot), //root as a rvalue
      { 0x80000065, 0x80000020 },
      passphrase,
      SecureBinaryData::fromString("control"),
      5); //set lookup computation to 5 entries

   //register with db
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //grab enough addresses to trigger a lookup extention
   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 5);

   for (unsigned i = 0; i < 10; i++)
      assetWlt->getNewAddress();
   auto newAddr = assetWlt->getNewAddress();

   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 105);

   {
      ////spend 27 from wlt to newAddr
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr->getRecipient(spendVal));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //register new wallet
   vector<BinaryData> hashVec;
   hashVec.push_back(newAddr->getPrefixedHash());
   auto newAddr2 = assetWlt->getNewAddress();
   hashVec.push_back(newAddr2->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());

   //mine some blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);

   //spend from the new address
   {
      //spend from the new address

      auto spendVal = 27 * COIN;
      Signer signer;

      auto feed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr2->getRecipient(spendVal));

      //sign, verify then broadcast
      {
         auto passlbd = [passphrase](const set<BinaryData>&)->SecureBinaryData
         {
            return passphrase;
         };

         assetWlt->setPassphrasePromptLambda(passlbd);
         auto lock = assetWlt->lockDecryptedContainer();
         signer.sign();
      }

      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_FromExtendedAddress_Salted)
{
   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto passphrase = SecureBinaryData::fromString("test");
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromSeed_BIP32_Blank(
      homedir_,
      wltRoot, //root as a rvalue
      passphrase,
      SecureBinaryData::fromString("control"));

   auto&& salt = CryptoPRNG::generateRandom(32);
   auto saltedAccType =
      make_shared<AccountType_BIP32_Salted>(salt);
   saltedAccType->setAddressLookup(5);
   saltedAccType->setDefaultAddressType(
      AddressEntryType_P2WPKH);
   saltedAccType->setAddressTypes(
      { AddressEntryType_P2WPKH });
   saltedAccType->setMain(true);

   auto passphraseLbd = [&passphrase](const set<BinaryData>&)->SecureBinaryData
   {
      return passphrase;
   };
   assetWlt->setPassphrasePromptLambda(passphraseLbd);

   //add salted account
   auto accountID = assetWlt->createBIP32Account(
      nullptr, {0x80000099, 0x80000001}, saltedAccType);

   assetWlt->resetPassphrasePromptLambda();

   //register with db
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //grab enough addresses to trigger a lookup extention
   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 5);

   for (unsigned i = 0; i < 10; i++)
      assetWlt->getNewAddress();
   auto newAddr = assetWlt->getNewAddress();

   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 105);

   {
      ////spend 27 from wlt to newAddr
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr->getRecipient(spendVal));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //register new wallet
   vector<BinaryData> hashVec;
   hashVec.push_back(newAddr->getPrefixedHash());
   auto newAddr2 = assetWlt->getNewAddress();
   hashVec.push_back(newAddr2->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());

   //mine some blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);

   //spend from the new address
   {
      //spend from the new address

      auto spendVal = 27 * COIN;
      Signer signer;

      auto feed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(newAddr2->getRecipient(spendVal));

      //sign, verify then broadcast
      {
         auto passlbd = [passphrase](const set<BinaryData>&)->SecureBinaryData
         {
            return passphrase;
         };

         assetWlt->setPassphrasePromptLambda(passlbd);
         auto lock = assetWlt->lockDecryptedContainer();
         signer.sign();
      }

      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SignerTest, SpendTest_FromExtendedAddress_ECDH)
{
   //ecdh account base key pair
   auto&& privKey = READHEX(
      "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F");
   auto&& pubKey = CryptoECDSA().ComputePublicKey(privKey, true);

   //create spender lamba
   auto getSpenderPtr = [](
      const UnspentTxOut& utxo,
      shared_ptr<ResolverFeed> feed)
      ->shared_ptr<ScriptSpender>
   {
      UTXO entry(utxo.value_, utxo.txHeight_, utxo.txIndex_, utxo.txOutIndex_,
         move(utxo.txHash_), move(utxo.script_));

      return make_shared<ScriptSpender>(entry, feed);
   };

   //setup bdm
   TestUtils::setBlocks({ "0", "1", "2", "3" }, blk0dat_);

   initBDM();

   theBDMt_->start(config.initMode_);
   auto&& bdvID = DBTestUtils::registerBDV(
      clients_, NetworkConfig::getMagicBytes());

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);

   //// create assetWlt ////

   //create a root private key
   auto passphrase = SecureBinaryData::fromString("test");
   auto&& wltRoot = CryptoPRNG::generateRandom(32);
   auto assetWlt = AssetWallet_Single::createFromSeed_BIP32_Blank(
      homedir_,
      wltRoot, //root as a rvalue
      passphrase,
      SecureBinaryData::fromString("control"));

   auto ecdhAccType = make_shared<AccountType_ECDH>(privKey, pubKey);
   ecdhAccType->setDefaultAddressType(
      AddressEntryType_P2WPKH);
   ecdhAccType->setAddressTypes(
      { AddressEntryType_P2WPKH });
   ecdhAccType->setMain(true);

   auto passphraseLbd = [&passphrase](const set<BinaryData>&)->SecureBinaryData
   {
      return passphrase;
   };

   //add salted account
   assetWlt->setPassphrasePromptLambda(passphraseLbd);
   auto addrAccountObj = assetWlt->createAccount(ecdhAccType);
   assetWlt->resetPassphrasePromptLambda();

   //register with db
   DBTestUtils::registerWallet(clients_, bdvID, scrAddrVec, "wallet1");
   auto bdvPtr = DBTestUtils::getBDV(clients_, bdvID);

   //wait on signals
   DBTestUtils::goOnline(clients_, bdvID);
   DBTestUtils::waitOnBDMReady(clients_, bdvID);
   auto wlt = bdvPtr->getWalletOrLockbox(wallet1id);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 3);

   //check balances
   const ScrAddrObj* scrObj;
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 5 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);

   //generate some ECDH addresses
   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 0);

   auto accPtr = dynamic_pointer_cast<AssetAccount_ECDH>(
      addrAccountObj->getOuterAccount());
   ASSERT_NE(accPtr, nullptr);

   for (unsigned i = 0; i < 5; i++)
   {
      auto&& salt = CryptoPRNG::generateRandom(32);
      accPtr->addSalt(salt);
   }

   vector<shared_ptr<AddressEntry>> addrVec;
   for (unsigned i = 0; i < 5; i++)
      addrVec.push_back(assetWlt->getNewAddress());

   EXPECT_EQ(assetWlt->getMainAccountAssetCount(), 5);

   {
      ////spend 27 from wlt to newAddr
      ////send rest back to scrAddrA

      auto spendVal = 27 * COIN;
      Signer signer;

      //instantiate resolver feed overloaded object
      auto feed = make_shared<ResolverUtils::TestResolverFeed>();

      auto addToFeed = [feed](const BinaryData& key)->void
      {
         auto&& datapair = DBTestUtils::getAddrAndPubKeyFromPrivKey(key);
         feed->h160ToPubKey_.insert(datapair);
         feed->pubKeyToPrivKey_[datapair.second] = key;
      };

      addToFeed(TestChain::privKeyAddrB);
      addToFeed(TestChain::privKeyAddrC);
      addToFeed(TestChain::privKeyAddrD);
      addToFeed(TestChain::privKeyAddrE);

      //get utxo list for spend value
      auto&& unspentVec = wlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(addrVec[0]->getRecipient(spendVal));

      if (total > spendVal)
      {
         //deal with change, no fee
         auto changeVal = total - spendVal;
         auto recipientChange = make_shared<Recipient_P2PKH>(
            TestChain::scrAddrD.getSliceCopy(1, 20), changeVal);
         signer.addRecipient(recipientChange);
      }

      //sign, verify then broadcast
      signer.sign();
      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 55 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   //register new wallet
   vector<BinaryData> hashVec;
   hashVec.push_back(addrVec[0]->getPrefixedHash());
   hashVec.push_back(addrVec[1]->getPrefixedHash());

   DBTestUtils::registerWallet(clients_, bdvID, hashVec, assetWlt->getID());
   auto dbAssetWlt = bdvPtr->getWalletOrLockbox(assetWlt->getID());

   //mine some blocks
   DBTestUtils::mineNewBlock(theBDMt_, TestChain::addrC, 2);
   DBTestUtils::waitOnNewBlockSignal(clients_, bdvID);
   EXPECT_EQ(DBTestUtils::getTopBlockHeight(iface_, HEADERS), 5);

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);

   //spend from the new address
   {
      //spend from the new address

      auto spendVal = 27 * COIN;
      Signer signer;

      auto feed = make_shared<ResolverFeed_AssetWalletSingle>(assetWlt);

      //get utxo list for spend value
      auto&& unspentVec = dbAssetWlt->getSpendableTxOutListForValue(spendVal);

      vector<UnspentTxOut> utxoVec;
      uint64_t tval = 0;
      auto utxoIter = unspentVec.begin();
      while (utxoIter != unspentVec.end())
      {
         tval += utxoIter->getValue();
         utxoVec.push_back(*utxoIter);

         if (tval > spendVal)
            break;

         ++utxoIter;
      }

      //create script spender objects
      uint64_t total = 0;
      for (auto& utxo : utxoVec)
      {
         total += utxo.getValue();
         signer.addSpender(getSpenderPtr(utxo, feed));
      }

      //spend spendVal to newAddr
      signer.addRecipient(addrVec[1]->getRecipient(spendVal));

      //sign, verify then broadcast
      {
         auto passlbd = [passphrase](const set<BinaryData>&)->SecureBinaryData
         {
            return passphrase;
         };

         assetWlt->setPassphrasePromptLambda(passlbd);
         auto lock = assetWlt->lockDecryptedContainer();
         signer.sign();
      }

      EXPECT_TRUE(signer.verify());

      DBTestUtils::ZcVector zcVec;
      zcVec.push_back(signer.serialize(), 14000000);

      DBTestUtils::pushNewZc(theBDMt_, zcVec);
      DBTestUtils::waitOnNewZcSignal(clients_, bdvID);
   }

   //check balances
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrA);
   EXPECT_EQ(scrObj->getFullBalance(), 50 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrB);
   EXPECT_EQ(scrObj->getFullBalance(), 30 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrC);
   EXPECT_EQ(scrObj->getFullBalance(), 155 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrD);
   EXPECT_EQ(scrObj->getFullBalance(), 8 * COIN);
   scrObj = wlt->getScrAddrObjByKey(TestChain::scrAddrE);
   EXPECT_EQ(scrObj->getFullBalance(), 0 * COIN);

   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[0]);
   EXPECT_EQ(scrObj->getFullBalance(), 0);
   scrObj = dbAssetWlt->getScrAddrObjByKey(hashVec[1]);
   EXPECT_EQ(scrObj->getFullBalance(), 27 * COIN);
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// Now actually execute all the tests
////////////////////////////////////////////////////////////////////////////////
GTEST_API_ int main(int argc, char **argv)
{
#ifdef _MSC_VER
   _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

   WSADATA wsaData;
   WORD wVersion = MAKEWORD(2, 0);
   WSAStartup(wVersion, &wsaData);
#endif

   GOOGLE_PROTOBUF_VERIFY_VERSION;
   srand(time(0));
   std::cout << "Running main() from gtest_main.cc\n";

   // Setup the log file 
   STARTLOGGING("cppTestsLog.txt", LogLvlDebug2);
   //LOGDISABLESTDOUT();
   btc_ecc_start();

   testing::InitGoogleTest(&argc, argv);
   int exitCode = RUN_ALL_TESTS();
   btc_ecc_stop();

   FLUSHLOG();
   CLEANUPLOG();
   google::protobuf::ShutdownProtobufLibrary();

   return exitCode;
}
