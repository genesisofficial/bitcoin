﻿// Copyright (c) 2014-2018 The Dash Core developers
// Copyright (c) 2014-2018 The Machinecoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternodes/activemasternode.h>
#include <addrman.h>
#include <base58.h>
#include <clientversion.h>
#include <masternodes/governance.h>
#include <masternodes/masternode-payments.h>
#include <masternodes/masternode-sync.h>
#include <masternodes/masternodeman.h>
#include <masternodes/messagesigner.h>
#include <netmessagemaker.h>
#include <masternodes/netfulfilledman.h>
#include <script/standard.h>
#include <ui_interface.h>
#include <util.h>
#include <warnings.h>

/** Masternode manager */
CMasternodeMan mnodeman;

const std::string CMasternodeMan::SERIALIZATION_VERSION_STRING = CLIENT_NAME + "-CMasternodeMan-Version-1";
const int CMasternodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, const CMasternode*>& t1,
                    const std::pair<int, const CMasternode*>& t2) const
    {
        if (t1.first != t2.first)
        {
            return t1.first < t2.first;
        }
        else
        {
            if (t1.second->activationBlockHeight != t2.second->activationBlockHeight)
            {
                return t1.second->activationBlockHeight < t2.second->activationBlockHeight;
            }
            else
            {
                return t1.second->outpoint < t2.second->outpoint;
            }            
        }
        
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};


struct CompareScoreMN
{
    bool operator()(const std::pair<arith_uint256, const CMasternode*>& t1,
                    const std::pair<arith_uint256, const CMasternode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareByAddr

{
    bool operator()(const CMasternode* t1,
                    const CMasternode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

CMasternodeMan::CMasternodeMan():
    cs(),
    mapMasternodes(),
    mAskedUsForMasternodeList(),
    mWeAskedForMasternodeList(),
    mWeAskedForMasternodeListEntry(),
    mWeAskedForVerification(),
    mMnbRecoveryRequests(),
    mMnbRecoveryGoodReplies(),
    listScheduledMnbRequestConnections(),
    fMasternodesAdded(false),
    fMasternodesRemoved(false),
    vecDirtyGovernanceObjectHashes(),
    nLastSentinelPingTime(0),
    mapSeenMasternodeBroadcast(),
    mapSeenMasternodePing()
{}

bool CMasternodeMan::Add(CMasternode &mn)
{
    LOCK(cs);

    if (Has(mn.outpoint))
    {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::Add -- NOT Adding new Masternode (already exists): addr=%s, %i now\n", mn.addr.ToString(), size());
        return false;
    } 

    LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::Add -- Adding new Masternode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
    if (mn.activationBlockHeight == 0)
    {
        Coin coin;
        if (GetUTXOCoin(mn.outpoint, coin)) {
            mn.activationBlockHeight = coin.nHeight;
        }        
    }
    mapMasternodes[mn.outpoint] = mn;
    fMasternodesAdded = true;
    return true;
}

void CMasternodeMan::AskForMN(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    if (!pnode)
    {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::AskForMN -- pnode is invalid \n");
        return;
    } 
        

    LOCK(cs);

    CService addrSquashed = CService(pnode->addr, 0);
    auto it1 = mWeAskedForMasternodeListEntry.find(outpoint);
    if (it1 != mWeAskedForMasternodeListEntry.end()) {
        auto it2 = it1->second.find(addrSquashed);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::AskForMN -- Skip (Last Request Too Recent): %s %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::AskForMN -- Asking same peer %s for missing masternode entry again: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::AskForMN -- Asking new peer %s for missing masternode entry: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::AskForMN -- Asking peer %s for missing masternode entry for the first time: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
    }
    mWeAskedForMasternodeListEntry[outpoint][addrSquashed] = GetTime() + DSEG_UPDATE_SECONDS;

    const CNetMsgMaker msgMaker(pnode->GetSendVersion());
    // if (pnode->GetSendVersion() == 70021) {
    //     connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, CTxIn(outpoint)));
    // } else {
    //     connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, outpoint));
    // }
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, outpoint));
}

bool CMasternodeMan::PoSeBan(const COutPoint &outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::PoSeBan -- Masternode not found \n");
        return false;
    }
    pmn->PoSeBan();

    return true;
}

void CMasternodeMan::Check()
{
    LOCK2(cs_main, cs);

    LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::Check -- nLastSentinelPingTime=%d, IsSentinelPingActive()=%d\n", nLastSentinelPingTime, IsSentinelPingActive());

    for (auto& mnpair : mapMasternodes) {
        // NOTE: internally it checks only every Params().GetConsensus().nMasternodeCheckSeconds seconds
        // since the last time, so expect some MNs to skip this
        mnpair.second.Check();
    }
}

void CMasternodeMan::CheckAndRemove(CConnman& connman)
{
    if (!masternodeSync.IsMasternodeListSynced())
    {
        LogPrintG(BCLogLevel::LOG_WARNING, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Masternode list is not synced \n");
        return;
    } 

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateMasternodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent masternodes, prepare structures and make requests to reasure the state of inactive ones
        rank_pair_vec_t vecMasternodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES masternode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        std::map<COutPoint, CMasternode>::iterator it = mapMasternodes.begin();
        while (it != mapMasternodes.end()) {
            CMasternodeBroadcast mnb = CMasternodeBroadcast(it->second);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if (it->second.IsOutpointSpent()) {
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Removing Masternode: %s  addr=%s  %i now\n", it->second.GetStateString(), it->second.addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenMasternodeBroadcast.erase(hash);
                mWeAskedForMasternodeListEntry.erase(it->first);

                // and finally remove it from the list
                it->second.FlagGovernanceItemsAsDirty();
                mapMasternodes.erase(it++);
                fMasternodesRemoved = true;
            } else {
                bool fAsk = (nAskForMnbRecovery > 0) &&
                            masternodeSync.IsSynced() &&
                            it->second.IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash) &&
                            !gArgs.IsArgSet("-connect");
                
                if (fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CService> setRequested;
                    // calulate only once and only when it's needed
                    if (vecMasternodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(nCachedBlockHeight);
                        GetMasternodeRanks(vecMasternodeRanks, nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL masternodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecMasternodeRanks.size(); i++) {
                        // avoid banning
                        if (mWeAskedForMasternodeListEntry.count(it->first) && mWeAskedForMasternodeListEntry[it->first].count(vecMasternodeRanks[i].second.addr))
                        { 
                            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Avoiding banning, masternode=%s\n", it->first.ToStringShort());
                            continue; 
                        }
                        // didn't ask recently, ok to ask now
                        CService addr = vecMasternodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if (fAskedForMnbRecovery) {
                        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Recovery initiated, masternode=%s\n", it->first.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for MASTERNODE_NEW_START_REQUIRED masternodes
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CMasternodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if (mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if (itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- reprocessing mnb, masternode=%s\n", itMnbReplies->second[0].outpoint.ToStringShort());
                    // mapSeenMasternodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateMasternodeList(NULL, itMnbReplies->second[0], nDos, connman);
                }
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- removing mnb recovery reply, masternode=%s, size=%d\n", itMnbReplies->second[0].outpoint.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        auto itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in MASTERNODE_NEW_START_REQUIRED state.
            if (GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Masternode list
        auto it1 = mAskedUsForMasternodeList.begin();
        while(it1 != mAskedUsForMasternodeList.end()){
            if ((*it1).second < GetTime()) {
                mAskedUsForMasternodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Masternode list
        it1 = mWeAskedForMasternodeList.begin();
        while(it1 != mWeAskedForMasternodeList.end()){
            if ((*it1).second < GetTime()){
                mWeAskedForMasternodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Masternodes we've asked for
        auto it2 = mWeAskedForMasternodeListEntry.begin();
        while(it2 != mWeAskedForMasternodeListEntry.end()){
            auto it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if (it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if (it2->second.empty()) {
                mWeAskedForMasternodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        auto it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if (it3->second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenMasternodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenMasternodePing
        std::map<uint256, CMasternodePing>::iterator it4 = mapSeenMasternodePing.begin();
        while(it4 != mapSeenMasternodePing.end()){
            if ((*it4).second.IsExpired()) {
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Removing expired Masternode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenMasternodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenMasternodeVerification
        std::map<uint256, CMasternodeVerification>::iterator itv2 = mapSeenMasternodeVerification.begin();
        while(itv2 != mapSeenMasternodeVerification.end()){
            if ((*itv2).second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS){
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckAndRemove -- Removing expired Masternode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenMasternodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }
    }

    if (fMasternodesRemoved) {
        NotifyMasternodeUpdates(connman);
    }
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    mapSeenMasternodeBroadcast.clear();
    mapSeenMasternodePing.clear();
    nLastSentinelPingTime = 0;
}

int CMasternodeMan::CountMasternodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.nProtocolVersion < nProtocolVersion)
        { 
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CountMasternodes -- Skip (Protocol version too low)\n");
            continue; 
        }
        nCount++;
    }

    return nCount;
}

int CMasternodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.nProtocolVersion < nProtocolVersion || !mnpair.second.IsEnabled())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CountEnabled -- Skip (Protocol version too low or masternode not enabled)\n");
            continue; 
        }
        nCount++;
    }

    return nCount;
}

int CMasternodeMan::CountCollateralisedAtHeight(int blockHeight)
{
    return CountCollateralisedAtHeight(mnpayments.GetMinMasternodePaymentsProto(), blockHeight, true);
}

int CMasternodeMan::CountCollateralisedAtHeight(int nProtocolVersion, int blockHeight, bool onlyEnabled)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.activationBlockHeight > blockHeight)
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CountCollateralisedAtHeight -- Skip (Activation block height too high)\n");
            continue; 
        }
        if (onlyEnabled && !mnpair.second.IsEnabled())
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CountCollateralisedAtHeight -- Skip (Masternode not enabled)\n");
            continue;
        }
        nCount++;
    }

    return nCount;
}

/* Only IPv4 masternodes are allowed in 12.1, saving this for later
int CMasternodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (const auto& mnpair : mapMasternodes)
        if ((nNetworkType == NET_IPV4 && mnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mnpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CMasternodeMan::DsegUpdate(CNode* pnode, CConnman& connman)
{
    LOCK(cs);

    CService addrSquashed = CService(pnode->addr, 0);
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            auto it = mWeAskedForMasternodeList.find(addrSquashed);
            if (it != mWeAskedForMasternodeList.end() && GetTime() < (*it).second) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", addrSquashed.ToString());
                return;
            }
        }
    }
  
    const CNetMsgMaker msgMaker(pnode->GetSendVersion());
    // if (pnode->GetSendVersion() == 70021) {
    //     connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, CTxIn()));
    // } else {
    //     connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, COutPoint()));
    // }
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, COutPoint()));

    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForMasternodeList[addrSquashed] = askAgain;

    LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
    LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DsegUpdate -- %s", pnode->addr.ToString());
}

CMasternode* CMasternodeMan::Find(const COutPoint &outpoint)
{
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    return it == mapMasternodes.end() ? NULL : &(it->second);
}

bool CMasternodeMan::Get(const COutPoint& outpoint, CMasternode& masternodeRet)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end()) {
        return false;
    }

    masternodeRet = it->second;
    return true;
}

bool CMasternodeMan::GetMasternodeInfo(const COutPoint& outpoint, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end()) {
        return false;
    }
    mnInfoRet = it->second.GetInfo();
    return true;
}

bool CMasternodeMan::GetMasternodeInfo(const CPubKey& pubKeyMasternode, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.pubKeyMasternode == pubKeyMasternode) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMasternodeMan::GetMasternodeInfo(const CScript& payee, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(WitnessV0KeyHash(mnpair.second.pubKeyCollateralAddress.GetID()));
        if (scriptCollateralAddress == payee) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMasternodeMan::GetMasternodeInfoFromCollateral(const CScript& payee, masternode_info_t& mnInfoRet)
{
    // Already locked?
    // LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(CScriptID(GetScriptForDestination(WitnessV0KeyHash(mnpair.second.pubKeyCollateralAddress.GetID()))));
        if (scriptCollateralAddress == payee) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMasternodeMan::GetMasternodeInfoFromCollateral(const CPubKey& pubKeyCollateralAddress, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.pubKeyCollateralAddress == pubKeyCollateralAddress) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

int CMasternodeMan::GetNodeActivationHeight(const CPubKey& pubKeyCollateralAddress)
{
    return GetNodeActivationHeight(GetScriptForDestination(WitnessV0KeyHash(pubKeyCollateralAddress.GetID())));
}

int CMasternodeMan::GetNodeActivationHeight(const CScript& payee) 
{
    // Already locked?
    //LOCK(cs);
    masternode_info_t primaryCheckMnInfo;
    if (GetMasternodeInfoFromCollateral(payee, primaryCheckMnInfo))
    {
        if (primaryCheckMnInfo.activationBlockHeight != 0)
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNodeActivationHeight -- Activation height from primaryCheckMnInfo\n");
            return primaryCheckMnInfo.activationBlockHeight;
        }
        else
        {
            // fix it the hard way then... dammit!
            Coin coin;
            if (GetUTXOCoin(primaryCheckMnInfo.outpoint, coin)) {
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNodeActivationHeight -- Activation height from GetUTXOCoin\n");
                return coin.nHeight;
            }
            else
            {
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNodeActivationHeight -- Still no activation height\n");
                return 0;
            }
        }            
    }
    else
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNodeActivationHeight -- Failed to get activation height from the masternode from payee\n");
        return 0;
    }
}

bool CMasternodeMan::Has(const COutPoint& outpoint)
{
    LOCK(cs);
    return mapMasternodes.find(outpoint) != mapMasternodes.end();
}

//
// Deterministically select the oldest/best masternode to pay on the network
//
bool CMasternodeMan::GetNextMasternodesInQueueForPayment(bool fFilterSigTime, int& nCountRet, masternode_info_t& mnInfoRet, std::vector<masternode_info_t>& vSecondaryMnInfoRet)
{
    return GetNextMasternodesInQueueForPayment(nCachedBlockHeight, fFilterSigTime, nCountRet, mnInfoRet, vSecondaryMnInfoRet);
}

bool CMasternodeMan::GetNextMasternodesInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, masternode_info_t& mnInfoRet, std::vector<masternode_info_t>& vSecondaryMnInfoRet)
{
    mnInfoRet = masternode_info_t();
    nCountRet = 0;

    if (!masternodeSync.IsWinnersListSynced()) {
        // without winner list we can't reliably find the next winner anyway
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment -- Skip (Winners list not synced)\n");
        return false;
    }

    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    std::vector<std::pair<int, const CMasternode*> > vecMasternodeLastPaid;
    std::vector<std::pair<int, const CMasternode*> > vecMasternodeLastPaidSecondary;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountMasternodes();

    // Primary
    for (const auto& mnpair : mapMasternodes) {
        if (!mnpair.second.IsValidForPayment())
        { 
            //LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] \n");
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Skip (Not Valid for Payment) \n");
            continue; 
        }

        //check protocol version
        if (mnpair.second.nProtocolVersion < mnpayments.GetMinMasternodePaymentsProto())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Skip (Protocol Version Too Low) \n");
            continue; 
        }

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (mnpayments.IsScheduled(mnpair.second, nBlockHeight))
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Skip (Scheduled for payment) \n");
            continue; 
        }

        //it's too new, wait for a cycle
        if (fFilterSigTime && mnpair.second.sigTime + (nMnCount*60) > GetAdjustedTime())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Skip (Too new) \n");
            continue; 
        }

        //make sure it has at least as many confirmations as there are masternodes
        if (GetUTXOConfirmations(mnpair.first) < nMnCount)
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Skip (Confirmation Count < Masternode Count) \n");
            continue; 
        }

        // Make sure that the activation height is set
        if (mnpair.second.activationBlockHeight <= 0)
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodePayments::FillBlockPayees -- Primary payee activation height <= zero. Eish. \n");
            continue; 
        }

        // Make sure the activation height is realistic
        if (mnpair.second.activationBlockHeight > nBlockHeight)
        {
            // Some kind of fair notice of what happened if this fails
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodePayments::FillBlockPayees -- Primary payee activation height is in the future... great Scott! \n");
            continue; 
        }

        // if (mnpair.second.nBlockLastPaidPrimary == 0)
        // {
        //     // This *could* be (but probably isn't) right... if this is is the first payment to this masternode
        //     int masternodesEnabledBlocks = nBlockHeight - Params().GetConsensus().nMasternodePaymentsStartBlock;
        //     int primaryPayeeBlockAge = nBlockHeight - mnpair.second.activationBlockHeight;
        //     bool irrationalPayeeAge = masternodesEnabledBlocks > nMnCount && primaryPayeeBlockAge > nMnCount;
        //     if (irrationalPayeeAge)
        //     {
        //         // This does not make sense, as this masternodes *should* have been paid by now
        //         LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodePayments::FillBlockPayees -- Primary payee nTimeLastPaidPrimary is irrationally zero \n");
        //         continue; 
        //     }
        // }

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment primary -- Selected \n");
        vecMasternodeLastPaid.push_back(std::make_pair(mnpair.second.GetLastPaidBlockPrimary(), &mnpair.second));
    }

    // Secondaries
    for (const auto& mnpair : mapMasternodes) {
        if (!mnpair.second.IsValidForPayment())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Skip (Not Valid for Payment) \n");
            continue; 
        }

        //check protocol version
        if (mnpair.second.nProtocolVersion < mnpayments.GetMinMasternodePaymentsProto())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Skip (Protocol Version Too Low) \n");
            continue; 
        }

        // it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        // skip this check for secondaries
        // if (mnpayments.IsScheduled(mnpair.second, nBlockHeight))
        // { 
        //     LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Skip (Scheduled for payment) \n");
        //     continue; 
        // }

        //it's too new, wait for a cycle
        if (fFilterSigTime && mnpair.second.sigTime + ((nMnCount/Params().GetConsensus().nMasternodeMaturitySecondariesMaxCount)*60) > GetAdjustedTime())
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Skip (Too new) \n");
            continue; 
        }

        //make sure it has at least as many confirmations as there are masternodes
        if (GetUTXOConfirmations(mnpair.first) < nMnCount)
        { 
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Skip (Confirmation Count < Masternode Count) \n");
            continue; 
        }

        // Add it to the secondaries list as well, but use the lastpaid secondary as the first term
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment secondary -- Selected \n");
        vecMasternodeLastPaidSecondary.push_back(std::make_pair(mnpair.second.GetLastPaidBlockSecondary(), &mnpair.second));
    }

    nCountRet = (int)vecMasternodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCountRet < nMnCount/3)
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::GetNextMasternodesInQueueForPayment -- Defer (Network upgrade)\n");
        return GetNextMasternodesInQueueForPayment(nBlockHeight, false, nCountRet, mnInfoRet, vSecondaryMnInfoRet);
    }

    // Sort them low to high
    // First primaries
    sort(vecMasternodeLastPaid.begin(), vecMasternodeLastPaid.end(), CompareLastPaidBlock());
    // then secondaries (the logic remains the same, but the input is different)
    sort(vecMasternodeLastPaidSecondary.begin(), vecMasternodeLastPaidSecondary.end(), CompareLastPaidBlock());

    // Calculate the primary
    uint256 blockHash;
    if (!GetBlockHash(blockHash, nBlockHeight - (Params().GetConsensus().nCoinbaseMaturity + 1))) {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternode::GetNextMasternodesInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - (Params().GetConsensus().nCoinbaseMaturity + 1));
        return false;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    const CMasternode *pBestMasternode = NULL;
    for (const auto& s : vecMasternodeLastPaid) {
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if (nScore > nHighest){
            nHighest = nScore;
            pBestMasternode = s.second;
        }
        nCountTenth++;
        if (nCountTenth >= nTenthNetwork) break;
    }
    if (pBestMasternode) {
        mnInfoRet = pBestMasternode->GetInfo();
    }

    // Now calculate the secondaries
    int secondariesToGet = Params().GetConsensus().nMasternodeMaturitySecondariesMaxCount;
    size_t sampleSize = 0;
    if ((int)vecMasternodeLastPaidSecondary.size() - 1 >= secondariesToGet)
    {
        sampleSize = secondariesToGet;
    }
    else
    {
        sampleSize = vecMasternodeLastPaidSecondary.size() - 1;
    }
    // copy sampleSize items to the output
    // <= to account for potentially skipping the one selected as a primary
    for(int i=0; i<= (int)sampleSize; ++i){
        const CMasternode *pBestSecondaryMasternode = vecMasternodeLastPaidSecondary[i].second;
        if (pBestSecondaryMasternode)
        {
            // make sure we do not add the primary to the secondaries list...
            if (pBestSecondaryMasternode !=  pBestMasternode)
            {
                masternode_info_t tempest = pBestSecondaryMasternode->GetInfo();
                vSecondaryMnInfoRet.push_back(tempest);
            }
        } 
        if (vSecondaryMnInfoRet.size() == sampleSize)
        {
            // do not take more than you need
            break;
        }
    }

    return mnInfoRet.fInfoValid;
}

masternode_info_t CMasternodeMan::FindRandomNotInVec(const std::vector<COutPoint> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- %d enabled masternodes, %d masternodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if (nCountNotExcluded < 1) return masternode_info_t();

    // fill a vector of pointers
    std::vector<const CMasternode*> vpMasternodesShuffled;
    for (const auto& mnpair : mapMasternodes) {
        vpMasternodesShuffled.push_back(&mnpair.second);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpMasternodesShuffled.begin(), vpMasternodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    for (const auto& pmn : vpMasternodesShuffled) {

        if (pmn->nProtocolVersion < nProtocolVersion)
        {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- Skip (Protocol Version Too Low) \n");
            continue; 
        }
        else if (!pmn->IsEnabled())
        { 
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- Skip (Not enabled) \n");
            continue; 
        }
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if (pmn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if (fExclude)
        { 
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- Skip (Excluded) \n");
            continue; 
        }

        // found the one not in vecToExclude
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- found, masternode=%s\n", pmn->outpoint.ToStringShort());
        return pmn->GetInfo();
    }

    LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::FindRandomNotInVec -- failed\n");
    return masternode_info_t();
}

bool CMasternodeMan::GetMasternodeScores(const uint256& nBlockHash, CMasternodeMan::score_pair_vec_t& vecMasternodeScoresRet, int nMinProtocol)
{
    vecMasternodeScoresRet.clear();

    if (!masternodeSync.IsMasternodeListSynced())
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeScores -- Skip (Masternode list not synced)\n");
        return false;
    }

    AssertLockHeld(cs);

    if (mapMasternodes.empty())
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeScores -- Skip (Masternode list empty)\n");
        return false;
    }

    // calculate scores
    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.nProtocolVersion >= nMinProtocol) {
            vecMasternodeScoresRet.push_back(std::make_pair(mnpair.second.CalculateScore(nBlockHash), &mnpair.second));
        }
    }

    sort(vecMasternodeScoresRet.rbegin(), vecMasternodeScoresRet.rend(), CompareScoreMN());
    return !vecMasternodeScoresRet.empty();
}

bool CMasternodeMan::GetMasternodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight, int nMinProtocol)
{
    nRankRet = -1;

    if (!masternodeSync.IsMasternodeListSynced())
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeRank -- Skip (Masternode list not synced)\n");
        return false;
    }

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(nBlockHash, vecMasternodeScores, nMinProtocol))
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeRank -- Skip (Unable to get masternode scores)\n");
        return false;
    }

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        if (scorePair.second->outpoint == outpoint) {
            nRankRet = nRank;
            return true;
        }
    }

    return false;
}

bool CMasternodeMan::GetMasternodeRanks(CMasternodeMan::rank_pair_vec_t& vecMasternodeRanksRet, int nBlockHeight, int nMinProtocol)
{
    vecMasternodeRanksRet.clear();

    if (!masternodeSync.IsMasternodeListSynced())
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeRanks -- Skip (Masternode list not synced)\n");
        return false;
    }
    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeRanks %s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(nBlockHash, vecMasternodeScores, nMinProtocol))
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::GetMasternodeRanks -- Skip (Unable to get masternode scores)\n");
        return false;
    }

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        vecMasternodeRanksRet.push_back(std::make_pair(nRank, *scorePair.second));
    }

    return true;
}

void CMasternodeMan::ProcessMasternodeConnections(CConnman& connman)
{
    //we don't care about this for regtest
    if (Params().NetworkIDString() == CBaseChainParams::REGTEST)
    {
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMasternodeConnections -- Skipped (RegTest) \n");
        return;
    }

    connman.ForEachNode(CConnman::AllNodes, [](CNode* pnode) {
#ifdef ENABLE_WALLET
        if (pnode->fMasternode) {
#else
        if (pnode->fMasternode) {
#endif // ENABLE_WALLET
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] Closing Masternode connection: peer=%d, addr=%s\n", pnode->GetId(), pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}

std::pair<CService, std::set<uint256> > CMasternodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if (listScheduledMnbRequestConnections.empty()) {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::PopScheduledMnbRequestConnection -- Skip (listScheduledMnbRequestConnections is empty)\n");
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();
    
    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if (pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}

void CMasternodeMan::ProcessPendingMnbRequests(CConnman& connman)
{
    std::pair<CService, std::set<uint256> > p = PopScheduledMnbRequestConnection();
    if (!(p.first == CService() || p.second.empty())) {
        if (connman.IsMasternodeOrDisconnectRequested(p.first))
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMasternodeConnections -- Skipped (IsMasternodeOrDisconnectRequested) \n");
            return;
        }
        mapPendingMNB.insert(std::make_pair(p.first, std::make_pair(GetTime(), p.second)));
        connman.AddPendingMasternode(p.first);
    }

    std::map<CService, std::pair<int64_t, std::set<uint256> > >::iterator itPendingMNB = mapPendingMNB.begin();
    while (itPendingMNB != mapPendingMNB.end()) {
        bool fDone = connman.ForNode(itPendingMNB->first, [&](CNode* pnode) {
            // compile request vector
            std::vector<CInv> vToFetch;
            std::set<uint256>& setHashes = itPendingMNB->second.second;
            std::set<uint256>::iterator it = setHashes.begin();
            while(it != setHashes.end()) {
                if (*it != uint256()) {
                    vToFetch.push_back(CInv(MSG_MASTERNODE_ANNOUNCE, *it));
                    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] -- asking for mnb %s from addr=%s\n", it->ToString(), pnode->addr.ToString());
                }
                ++it;
            }

            // ask for data
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            return true;
        });

        int64_t nTimeAdded = itPendingMNB->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNB->first.ToString());
            }
            mapPendingMNB.erase(itPendingMNB++);
        } else {
            ++itPendingMNB;
        }
    }
    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] %s -- mapPendingMNB size: %d\n", __func__, mapPendingMNB.size());
}

void CMasternodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{  
    if (fLiteMode)
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage -- Skipped (Lite Mode Detected) \n");
        return;
    } // disable all Genesis masternode specific functionality

    if (strCommand == NetMsgType::MNANNOUNCE) { //Masternode Broadcast

        CMasternodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        if (!masternodeSync.IsBlockchainSynced())
        {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNANNOUNCE) -- Skipped (Blockchain not synced) \n");
            return;
        }

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] MNANNOUNCE -- Masternode announce, masternode=%s\n", mnb.outpoint.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateMasternodeList(pfrom, mnb, nDos, connman)) {
            // use announced Masternode as a peer
            std::vector<CAddress> vAddr;
            vAddr.push_back(CAddress(mnb.addr, NODE_NETWORK));
            connman.AddNewAddresses(vAddr, pfrom->addr, 2*60*60);
        } else if (nDos > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDos);
        }

        if (fMasternodesAdded) {
            NotifyMasternodeUpdates(connman);
        }
    } else if (strCommand == NetMsgType::MNPING) { //Masternode Ping        
        CMasternodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if (!masternodeSync.IsBlockchainSynced())
        {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Skipped (Blockchain not synced) \n");
            return;
        }

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Masternode ping, masternode=%s\n", mnp.masternodeOutpoint.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if (mapSeenMasternodePing.count(nHash))
        {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Skipped (Seen) \n");
            return;
        } //seen

        mapSeenMasternodePing.insert(std::make_pair(nHash, mnp));

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] MNPING -- Masternode ping, masternode=%s new\n", mnp.masternodeOutpoint.ToStringShort());

        // see if we have this Masternode
        CMasternode* pmn = Find(mnp.masternodeOutpoint);

        if (pmn && mnp.fSentinelIsCurrent)
            UpdateLastSentinelPingTime();

        // too late, new MNANNOUNCE is required
        if (pmn && pmn->IsNewStartRequired())
        {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Skipped (Too late, new MNANNOUNCE required) \n");
            return;
        }

        int nDos = 0;
        if (mnp.CheckAndUpdate(pmn, false, nDos, connman))
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Skipped (Updated) \n");
            return;
        }

        if (nDos > 0) {
            // if anything significant failed, mark that node
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNPING) -- Node is acting suspicious \n");
            Misbehaving(pfrom->GetId(), nDos);
        } else if (pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a masternode entry once
        AskForMN(pfrom, mnp.masternodeOutpoint, connman);

    } else if (strCommand == NetMsgType::DSEG) { //Get Masternode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after masternode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!masternodeSync.IsSynced())
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (DSEG) -- Skipped (Masternodes not synced) \n");
            return;
        }

        COutPoint masternodeOutpoint;

        // if (pfrom->nVersion == 70021) {
        //     CTxIn vin;
        //     vRecv >> vin;
        //     masternodeOutpoint = vin.prevout;
        // } else {
        //     vRecv >> masternodeOutpoint;
        // }
        vRecv >> masternodeOutpoint;
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] DSEG -- Masternode list, masternode=%s\n", masternodeOutpoint.ToStringShort());

        if (masternodeOutpoint.IsNull()) {
            SyncAll(pfrom, connman);
        } else {
            SyncSingle(pfrom, masternodeOutpoint, connman);
        }

    } else if (strCommand == NetMsgType::MNVERIFY) { // Masternode Verify

        // Need LOCK2 here to ensure consistent locking order because all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CMasternodeVerification mnv;
        vRecv >> mnv;

        pfrom->setAskFor.erase(mnv.GetHash());

        if (!masternodeSync.IsMasternodeListSynced())
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNVERIFY) -- Skipped (Mastenode list not synced) \n");
            return;
        }

        if (mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNVERIFY) -- Asked to verify myself \n");
            SendVerifyReply(pfrom, mnv, connman);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some masternode
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNVERIFY) -- (Probably) Got requested verification reply we requested from a masternode \n");
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some masternode which verified another one
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessMessage (MNVERIFY) -- (Probably) Got requested verification broadcast signed by some masternode which verified another one \n");
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}
                     
void CMasternodeMan::SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced())
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::SyncSingle -- Skipped (Masternodes not synced) \n");
        return;
    }

    LOCK(cs);

    auto it = mapMasternodes.find(outpoint);

    if (it != mapMasternodes.end()) {
        if (it->second.addr.IsRFC1918() || it->second.addr.IsLocal())
        {
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::SyncSingle -- Skipped (Local Network Masternode) \n");
            return;
        } // do not send local network masternode
        // NOTE: send masternode regardless of its current state, the other node will need it to verify old votes.
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- Sending Masternode entry: masternode=%s  addr=%s\n", __func__, outpoint.ToStringShort(), it->second.addr.ToString());
        PushDsegInvs(pnode, it->second);
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- Sent 1 Masternode inv to peer=%d\n", __func__, pnode->GetId());
    }
}

void CMasternodeMan::SyncAll(CNode* pnode, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced())
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::SyncAll -- Skipped (Masternodes not synced) \n");
        return;
    }

    // local network
    bool isLocal = (pnode->addr.IsRFC1918() || pnode->addr.IsLocal());

    CService addrSquashed = CService(pnode->addr, 0);
    // should only ask for this once
    if (!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
        LOCK2(cs_main, cs);
        auto it = mAskedUsForMasternodeList.find(addrSquashed);
        if (it != mAskedUsForMasternodeList.end() && it->second > GetTime()) {
            Misbehaving(pnode->GetId(), 34);
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- peer already asked me for the list, peer=%d\n", __func__, pnode->GetId());
            return;
        }
        int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
        mAskedUsForMasternodeList[addrSquashed] = askAgain;
    }

    int nInvCount = 0;

    LOCK(cs);

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.addr.IsRFC1918())
        { 
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::SyncAll -- Skip (IsRFC1918) \n");
            continue; 
        } 
        else if (mnpair.second.addr.IsLocal())
        { 
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::SyncAll -- Skip (IsLocal) \n");
            continue; 
        } // do not send local network masternode
        // NOTE: send masternode regardless of its current state, the other node will need it to verify old votes.
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- Sending Masternode entry: masternode=%s  addr=%s\n", __func__, mnpair.first.ToStringShort(), mnpair.second.addr.ToString());
        PushDsegInvs(pnode, mnpair.second);
        nInvCount++;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_LIST, nInvCount));
    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- Sent %d Masternode invs to peer=%d\n", __func__, nInvCount, pnode->GetId());
}

void CMasternodeMan::PushDsegInvs(CNode* pnode, const CMasternode& mn)
{
    AssertLockHeld(cs);

    CMasternodeBroadcast mnb(mn);
    CMasternodePing mnp = mnb.lastPing;
    uint256 hashMNB = mnb.GetHash();
    uint256 hashMNP = mnp.GetHash();
    pnode->PushInventory(CInv(MSG_MASTERNODE_ANNOUNCE, hashMNB));
    pnode->PushInventory(CInv(MSG_MASTERNODE_PING, hashMNP));
    mapSeenMasternodeBroadcast.insert(std::make_pair(hashMNB, std::make_pair(GetTime(), mnb)));
    mapSeenMasternodePing.insert(std::make_pair(hashMNP, mnp));
}

// Verification of masternodes via unique direct requests.

void CMasternodeMan::DoFullVerificationStep(CConnman& connman)
{
    if (activeMasternode.outpoint.IsNull())
    {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Skipped (Outpoint is Null) \n");
        return;
    }
    if (!masternodeSync.IsSynced())
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Skipped (Masternodes not synced) \n");
        return;
    }

    rank_pair_vec_t vecMasternodeRanks;
    GetMasternodeRanks(vecMasternodeRanks, nCachedBlockHeight - 1, MIN_POSE_PROTO_VERSION);

    LOCK(cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecMasternodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    rank_pair_vec_t::iterator it = vecMasternodeRanks.begin();
    while(it != vecMasternodeRanks.end()) {
        if (it->first > MAX_POSE_RANK) {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if (it->second.outpoint == activeMasternode.outpoint) {
            nMyRank = it->first;
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d masternodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this masternode is not enabled
    if (nMyRank == -1)
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Skipped (List is too short and this masternode is not enabled) \n");
        return;
    }

    // send verify requests to up to MAX_POSE_CONNECTIONS masternodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if (nOffset >= (int)vecMasternodeRanks.size())
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Skipped (Offset too large) \n");
        return;
    }

    std::vector<const CMasternode*> vSortedByAddr;
    for (const auto& mnpair : mapMasternodes) {
        vSortedByAddr.push_back(&mnpair.second);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecMasternodeRanks.begin() + nOffset;
    while(it != vecMasternodeRanks.end()) {
        if (it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Already %s%s%s masternode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.outpoint.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if (nOffset >= (int)vecMasternodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Verifying masternode %s rank %d/%d address %s\n",
                    it->second.outpoint.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if (SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr, connman)) {
            nCount++;
            if (nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if (nOffset >= (int)vecMasternodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::DoFullVerificationStep -- Sent verification requests to %d masternodes\n", nCount);
}

// This function tries to find masternodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CMasternodeMan::CheckSameAddr()
{
    if (!masternodeSync.IsSynced())
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- Skipped (Masternodes not synced) \n");
        return;
    }
    else if (mapMasternodes.empty())
    { 
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- Skipped (Masternodes map is empty) \n");
        return;
    }

    std::vector<CMasternode*> vBan;
    std::vector<CMasternode*> vSortedByAddr;

    {
        LOCK(cs);

        CMasternode* pprevMasternode = NULL;
        CMasternode* pverifiedMasternode = NULL;

        for (auto& mnpair : mapMasternodes) {
            vSortedByAddr.push_back(&mnpair.second);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        for (const auto& pmn : vSortedByAddr) {
            // check only (pre)enabled masternodes
            if (!pmn->IsEnabled() && !pmn->IsPreEnabled())
            { 
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- Skip (IsEnabled && !IsPreEnabled) \n");
                continue; 
            }
            // initial step
            if (!pprevMasternode) {
                pprevMasternode = pmn;
                pverifiedMasternode = pmn->IsPoSeVerified() ? pmn : NULL;
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- Skip (Initial Step) \n");
                continue;
            }
            // second+ step
            if (pmn->addr == pprevMasternode->addr) {
                if (pverifiedMasternode) {
                    // another masternode with the same ip is verified, ban this one
                    LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- Another masternode with the same ip is verified, ban this one \n");
                    vBan.push_back(pmn);
                } else if (pmn->IsPoSeVerified()) {
                    // this masternode with the same ip is verified, ban previous one
                    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- This masternode with the same ip is verified, ban previous one \n");
                    vBan.push_back(pprevMasternode);
                    // and keep a reference to be able to ban following masternodes with the same ip
                    pverifiedMasternode = pmn;
                }
            } else {
                pverifiedMasternode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevMasternode = pmn;
        }
    }

    // ban duplicates
    for (auto& pmn : vBan) {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckSameAddr -- increasing PoSe ban score for masternode %s\n", pmn->outpoint.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CMasternodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<const CMasternode*>& vSortedByAddr, CConnman& connman)
{
    if (netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    if (connman.IsMasternodeOrDisconnectRequested(addr))
    {
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyRequest -- Skipped (IsMasternodeOrDisconnectRequested) \n");
        return false;
    }
    
    connman.AddPendingMasternode(addr);
    // use random nonce, store it and require node to reply with correct one later
    CMasternodeVerification mnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    LOCK(cs_mapPendingMNV);
    mapPendingMNV.insert(std::make_pair(addr, std::make_pair(GetTime(), mnv)));
    LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());

    return true;
}
                         
void CMasternodeMan::ProcessPendingMnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingMNV);

    std::map<CService, std::pair<int64_t, CMasternodeVerification> >::iterator itPendingMNV = mapPendingMNV.begin();

    while (itPendingMNV != mapPendingMNV.end()) {
        bool fDone = connman.ForNode(itPendingMNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingMNV->second.second;
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] -- verifying node using nonce %d addr=%s\n", itPendingMNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(pnode->GetSendVersion()); // TODO this gives a warning about version not being set (we should wait for VERSION exchange)
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, itPendingMNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingMNV->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNV->first.ToString());
            }
            mapPendingMNV.erase(itPendingMNV++);
        } else {
            ++itPendingMNV;
        }
    }
    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] %s -- mapPendingMNV size: %d\n", __func__, mapPendingMNV.size());
}

void CMasternodeMan::SendVerifyReply(CNode* pnode, CMasternodeVerification& mnv, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // only masternodes can sign this, why would someone ask regular node?
    if (!fMasternodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyReply -- Only masternodes can sign a verified reply, but I am not a masternode... \n");
        return;
    }

    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] MasternodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] MasternodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    std::string strError;

    if (chainActive.Height() > Params().GetConsensus().nMasternodeSignHashThreshold) {
        uint256 hash = mnv.GetSignatureHash1(blockHash);
        if (!CHashSigner::SignHash(hash, activeMasternode.keyMasternode, mnv.vchSig1)) {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyReply -- SignHash() failed\n");
            return;
        }
        if (!CHashSigner::VerifyHash(hash, activeMasternode.pubKeyMasternode, mnv.vchSig1, strError)) {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyReply -- VerifyHash() failed, error: %s\n", strError);
            return;
        }
    } else {
        std::string strMessage = strprintf("%s%d%s", activeMasternode.service.ToString(), mnv.nonce, blockHash.ToString());

        if (!CMessageSigner::SignMessage(strMessage, mnv.vchSig1, activeMasternode.keyMasternode)) {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyReply -- SignMessage() failed\n");
            return;
        }

        if (!CMessageSigner::VerifyMessage(activeMasternode.pubKeyMasternode, mnv.vchSig1, strMessage, strError)) {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
            return;
        }
    }

    const CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, mnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CMasternodeMan::ProcessVerifyReply(CNode* pnode, CMasternodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if (!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] MasternodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    // we already verified this address, why node is spamming?
    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    {
        LOCK(cs);

        CMasternode* prealMasternode = NULL;
        std::vector<CMasternode*> vpMasternodesToBan;
        
        uint256 hash1 = mnv.GetSignatureHash1(blockHash);
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        for (auto& mnpair : mapMasternodes) {
            if (CAddress(mnpair.second.addr, NODE_NETWORK) == pnode->addr) {
                bool fFound = false;
                if (chainActive.Height() > Params().GetConsensus().nMasternodeSignHashThreshold) {
                    fFound = CHashSigner::VerifyHash(hash1, mnpair.second.pubKeyMasternode, mnv.vchSig1, strError);
                    // we don't care about mnv with signature in old format
                } else {
                    fFound = CMessageSigner::VerifyMessage(mnpair.second.pubKeyMasternode, mnv.vchSig1, strMessage1, strError);
                }
                // we don't care about mnv with signature in old format
                if (fFound) {
                    // found it!
                    prealMasternode = &mnpair.second;
                    if (!mnpair.second.IsPoSeVerified()) {
                        mnpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated masternode
                    if (activeMasternode.outpoint.IsNull())
                    { 
                        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- Skip (Active masternode outpoint is null) \n");
                        continue; 
                    }
                    // update ...
                    mnv.addr = mnpair.second.addr;
                    mnv.masternodeOutpoint1 = mnpair.second.outpoint;
                    mnv.masternodeOutpoint2 = activeMasternode.outpoint;
                    // ... and sign it

                    std::string strError;

                    if (chainActive.Height() > Params().GetConsensus().nMasternodeSignHashThreshold) {
                        uint256 hash2 = mnv.GetSignatureHash2(blockHash);

                        if (!CHashSigner::SignHash(hash2, activeMasternode.keyMasternode, mnv.vchSig2)) {
                            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- SignHash() failed\n");
                            return;
                        }

                        if (!CHashSigner::VerifyHash(hash2, activeMasternode.pubKeyMasternode, mnv.vchSig2, strError)) {
                            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- VerifyHash() failed, error: %s\n", strError);
                            return;
                        }
                    } else {
                        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                                mnv.masternodeOutpoint1.ToStringShort(), mnv.masternodeOutpoint2.ToStringShort());

                        if (!CMessageSigner::SignMessage(strMessage2, mnv.vchSig2, activeMasternode.keyMasternode)) {
                            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                            return;
                        }

                        if (!CMessageSigner::VerifyMessage(activeMasternode.pubKeyMasternode, mnv.vchSig2, strMessage2, strError)) {
                            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                            return;
                        }
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mapSeenMasternodeVerification.insert(std::make_pair(mnv.GetHash(), mnv));
                    mnv.Relay();

                } else {
                    vpMasternodesToBan.push_back(&mnpair.second);
                }
            }
        }
        // no real masternode found?...
        if (!prealMasternode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- ERROR: no real masternode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->GetId(), 20);
            return;
        }
        LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- verified real masternode %s for addr %s\n",
                    prealMasternode->outpoint.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        for (const auto& pmn : vpMasternodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealMasternode->outpoint.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        if (!vpMasternodesToBan.empty())
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyReply -- PoSe score increased for %d fake masternodes, addr %s\n",
                        (int)vpMasternodesToBan.size(), pnode->addr.ToString());
    }
}

void CMasternodeMan::ProcessVerifyBroadcast(CNode* pnode, const CMasternodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    if (mapSeenMasternodeVerification.find(mnv.GetHash()) != mapSeenMasternodeVerification.end()) {
        // we already have one
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Skip (We already have a verification) \n");
        return;
    }
    mapSeenMasternodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if (mnv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    nCachedBlockHeight, mnv.nBlockHeight, pnode->GetId());
        return;
    }

    if (mnv.masternodeOutpoint1 == mnv.masternodeOutpoint2) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- ERROR: same outpoints %s, peer=%d\n",
                    mnv.masternodeOutpoint1.ToStringShort(), pnode->GetId());
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->GetId(), 100);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    int nRank;

    if (!GetMasternodeRank(mnv.masternodeOutpoint2, nRank, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Can't calculate rank for masternode %s\n",
                    mnv.masternodeOutpoint2.ToStringShort());
        return;
    }

    if (nRank > MAX_POSE_RANK) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Masternode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.masternodeOutpoint2.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->GetId());
        return;
    }

    {
        LOCK(cs);

        CMasternode* pmn1 = Find(mnv.masternodeOutpoint1);
        if (!pmn1) {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- can't find masternode1 %s\n", mnv.masternodeOutpoint1.ToStringShort());
            return;
        }

        CMasternode* pmn2 = Find(mnv.masternodeOutpoint2);
        if (!pmn2) {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- can't find masternode2 %s\n", mnv.masternodeOutpoint2.ToStringShort());
            return;
        }

        if (pmn1->addr != mnv.addr) {
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- addr %s does not match %s\n", mnv.addr.ToString(), pmn1->addr.ToString());
            return;
        }

        if (chainActive.Height() > Params().GetConsensus().nMasternodeSignHashThreshold) {
            uint256 hash1 = mnv.GetSignatureHash1(blockHash);
            uint256 hash2 = mnv.GetSignatureHash2(blockHash);

            if (!CHashSigner::VerifyHash(hash1, pmn1->pubKeyMasternode, mnv.vchSig1, strError)) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }

            if (!CHashSigner::VerifyHash(hash2, pmn2->pubKeyMasternode, mnv.vchSig2, strError)) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }
        } else {
            std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
            std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                    mnv.masternodeOutpoint1.ToStringShort(), mnv.masternodeOutpoint2.ToStringShort());

            if (!CMessageSigner::VerifyMessage(pmn1->pubKeyMasternode, mnv.vchSig1, strMessage1, strError)) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- VerifyMessage() for masternode1 failed, error: %s\n", strError);
                return;
            }

            if (!CMessageSigner::VerifyMessage(pmn2->pubKeyMasternode, mnv.vchSig2, strMessage2, strError)) {
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- VerifyMessage() for masternode2 failed, error: %s\n", strError);
                return;
            }
        }

        if (!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- verified masternode %s for addr %s\n",
                    pmn1->outpoint.ToStringShort(), pmn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& mnpair : mapMasternodes) {
            if (mnpair.second.addr != mnv.addr)
            { 
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Skip (Addresses do not match) \n");
                continue; 
            } else if (mnpair.first == mnv.masternodeOutpoint1)
            { 
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- Skip (Outpoints match) \n");
                continue; 
            }
            mnpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mnpair.first.ToStringShort(), mnpair.second.addr.ToString(), mnpair.second.nPoSeBanScore);
        }
        if (nCount)
            LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake masternodes, addr %s\n",
                        nCount, pmn1->addr.ToString());
    }
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "Masternodes: " << (int)mapMasternodes.size() <<
            ", peers who asked us for Masternode list: " << (int)mAskedUsForMasternodeList.size() <<
            ", peers we asked for Masternode list: " << (int)mWeAskedForMasternodeList.size() <<
            ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size();

    return info.str();
}

bool CMasternodeMan::CheckMnbAndUpdateMasternodeList(CNode* pfrom, CMasternodeBroadcast mnb, int& nDos, CConnman& connman)
{
    // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s\n", mnb.outpoint.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenMasternodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen\n", mnb.outpoint.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenMasternodeBroadcast[hash].first > Params().GetConsensus().nMasternodeNewStartRequiredSeconds - Params().GetConsensus().nMasternodeMinMnpSeconds * 2) {
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen update\n", mnb.outpoint.ToStringShort());
                mapSeenMasternodeBroadcast[hash].first = GetTime();
                masternodeSync.BumpAssetLastTime("CMasternodeMan::CheckMnbAndUpdateMasternodeList - seen");
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenMasternodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CMasternode mnTemp = CMasternode(mnb);
                        mnTemp.Check();
                        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - mnb.lastPing.sigTime)/60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen good\n", mnb.outpoint.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenMasternodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s new\n", mnb.outpoint.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- SimpleCheck() failed, masternode=%s\n", mnb.outpoint.ToStringShort());
            return false;
        }

        // search Masternode list
        CMasternode* pmn = Find(mnb.outpoint);
        if (pmn) {
            CMasternodeBroadcast mnbOld = mapSeenMasternodeBroadcast[CMasternodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos, connman)) {
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Update() failed, masternode=%s\n", mnb.outpoint.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenMasternodeBroadcast.erase(mnbOld.GetHash());
            }
            return true;
        }
    }

    if (mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        masternodeSync.BumpAssetLastTime("CMasternodeMan::CheckMnbAndUpdateMasternodeList - new");
        // if it matches our Masternode privkey...
        if (fMasternodeMode && mnb.pubKeyMasternode == activeMasternode.pubKeyMasternode) {
            mnb.nPoSeBanScore = -Params().GetConsensus().nMasternodePoseBanMaxScore;
            if (mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Got NEW Masternode entry: masternode=%s  sigTime=%lld  addr=%s\n",
                            mnb.outpoint.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeMasternode.ManageState(connman);
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.Relay(connman);
    } else {
        LogPrintG(BCLogLevel::LOG_ERROR, BCLog::MN, "[Masternodes] CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Rejected Masternode entry: %s  addr=%s\n", mnb.outpoint.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

// This is a simplified version of what is done, when UpdateLastPaid is called for each masternode
void CMasternodeMan::UpdateLastPaidGlobal(const CBlockIndex* pindex, int nMaxBlocksToScanBack)
{
    if (!pindex) return;

    // Make our own list to make lookups easier
    std::map<CScript, CMasternode> localNodeMap;
    for (const auto& mnpair : mapMasternodes) {
        CScript mnpayee = GetScriptForDestination(CScriptID(GetScriptForDestination(WitnessV0KeyHash(mnpair.second.pubKeyCollateralAddress.GetID()))));
        localNodeMap[mnpayee] = mnpair.second;
    }
    
    const CBlockIndex *pindexActive = chainActive.Tip();
    assert(pindexActive);

    CDiskBlockPos blockPos = pindexActive->GetBlockPos();
    int maxSecondaryCount = Params().GetConsensus().nMasternodeMaturitySecondariesMaxCount;

    // Simplified... always go as far back as we can
    for (int i = 0; i < nMaxBlocksToScanBack; i++) {
        size_t checkitOut = mnpayments.mapMasternodeBlocksPrimary.count(pindexActive->nHeight);
        int mnCount = CountMasternodes(-1);

        if ((mnCount > 2 && checkitOut) || (mnCount <= 2))
        {
            if (blockPos.IsNull() == true) {
                return;
            }

            CBlock block;
            if (!ReadBlockFromDisk(block, blockPos, Params().GetConsensus()))
            {
                continue;
            }

            // Check that we are not wasting our time with a block that has no MN payments
            if (block.vtx[0]->vout.size() < 7)
            {
                continue;
            }

            // Explained in the individual masternode run version.
            int primaryMnPaymentPosition = 6; // starting from 0
            // Setting this to -1, so the initial increment sets it to 0.
            int positionTracker = -1;
            for (const auto& txout : block.vtx[0]->vout)
            {
                positionTracker++;
                if (positionTracker < primaryMnPaymentPosition)
                {
                    continue;
                }

                // If we've made it this far, get some basic info about the tx
                CScript txPayee = txout.scriptPubKey;
                int nHeight = pindexActive->nHeight;
                uint32_t nTime = pindexActive->nTime;
                // Once we enforce the amount and the primary payee... do this:
                // CAmount txValue = txout.nValue;
                // CAmount nMasternodePaymentPrimary = GetMasternodePayments(pindexActive->nHeight, activationBlockHeight, block.vtx[0]->GetValueOut());
                // double readableMnPayValue = nMasternodePaymentPrimary / COIN;
                
                // check if we have this mn... otherwise the rest doesn't make sense
                auto findIt = localNodeMap.find(txPayee);
                if (findIt == localNodeMap.end())
                {
                    // we don't got this mn... nothing to see here... move along!
                    continue;
                }

                if (positionTracker == primaryMnPaymentPosition)
                {
                    // this is the spot for the primary payment
                    // these values were off initially. :(
                    // But a payment was made...
                    // The value must be enforced when the dust settles.
                    // Anyhow...

                    // mark as a primary payment if needed
                    if (findIt->second.nBlockLastPaidPrimary < nHeight)
                    {
                        // set it in the local list
                        findIt->second.nBlockLastPaidPrimary = nHeight;
                        findIt->second.nTimeLastPaidPrimary = nTime;
                        // and in the global list
                        auto globalListIt = mapMasternodes.find(findIt->second.outpoint);
                        if (globalListIt != mapMasternodes.end())
                        {
                            globalListIt->second.nBlockLastPaidPrimary = nHeight;
                            globalListIt->second.nTimeLastPaidPrimary = nTime;
                        }
                    }
                }
                // accounting for 20 secondaries and counting from 0... anything after that cannot be a masternode payment anyway
                else if (positionTracker > primaryMnPaymentPosition && positionTracker < (primaryMnPaymentPosition + maxSecondaryCount + 1)) 
                {
                    // From here on, it is a secondary payment
                    // mark as a secondary payment if needed
                    if (findIt->second.nBlockLastPaidSecondary < nHeight)
                    {
                        // set it in the local list
                        findIt->second.nBlockLastPaidSecondary = nHeight;
                        findIt->second.nTimeLastPaidSecondary = nTime;
                        // and in the global list
                        auto globalListIt = mapMasternodes.find(findIt->second.outpoint);
                        if (globalListIt != mapMasternodes.end())
                        {
                            globalListIt->second.nBlockLastPaidSecondary = nHeight;
                            globalListIt->second.nTimeLastPaidSecondary = nTime;
                        }
                    }
                }
            }
        }
        
        // Go back to the previous block if we can... let's Rock!
        if (pindexActive->pprev == nullptr) { assert(pindexActive); break; }
        pindexActive = pindexActive->pprev;
    }


}

void CMasternodeMan::UpdateLastPaid(const CBlockIndex* pindex, bool lock)
{
    LOCK(cs);

    if (fLiteMode)
    { 
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::UpdateLastPaid -- Skipped (fLiteMode enabled) \n");
        return;
    }

    if (!masternodeSync.IsWinnersListSynced())
    { 
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::UpdateLastPaid -- Skipped (Winners list not synced) \n");
        return;
    }

    if (mapMasternodes.empty())
    { 
        LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::UpdateLastPaid -- Skipped (Masternodes map is empty) \n");
        return;
    }

    // Actually... go back as far as we can for now
    int nMaxBlocksToScanBack = 0;

    // use lastpaid to limit the number of blocks...
    if (nUpdateLastPaidBlock == 0)
    {
        // it has not run yet...
        nMaxBlocksToScanBack = mnpayments.GetStorageLimit();
    }
    else
    {
        // it has run, so we don't need to dive quite as deeply - add an extra block... just in case ;)
        nMaxBlocksToScanBack = (nCachedBlockHeight - nUpdateLastPaidBlock) + 1;
    }
    
    // static int nLastRunBlockHeight = 0;
    // Scan at least LAST_PAID_SCAN_BLOCKS but no more than mnpayments.GetStorageLimit()
    // int nMaxBlocksToScanBack = std::max(LAST_PAID_SCAN_BLOCKS, nCachedBlockHeight - nLastRunBlockHeight);
    // nMaxBlocksToScanBack = std::min(nMaxBlocksToScanBack, mnpayments.GetStorageLimit());

    LogPrintG(BCLogLevel::LOG_DEBUG, BCLog::MN, "[Masternodes] CMasternodeMan::UpdateLastPaid -- nCachedBlockHeight=%d, nUpdateLastPaidBlock=%d, nMaxBlocksToScanBack=%d\n",
                            nCachedBlockHeight, nUpdateLastPaidBlock, nMaxBlocksToScanBack);

    // This is fine when you have a 1:1 relationshaip between blocks and masternodes getting paid
    // but... we have multiple payees per block, so we need to rethink this, so we don't run through
    // the chain masternodeCount times.
    // for (auto& mnpair : mapMasternodes) {
    //     mnpair.second.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
    // }

    // Should be faster....
    UpdateLastPaidGlobal(pindex, nMaxBlocksToScanBack);

    nUpdateLastPaidBlock = nCachedBlockHeight;
}

void CMasternodeMan::UpdateLastSentinelPingTime()
{
    LOCK(cs);
    nLastSentinelPingTime = GetTime();
}

bool CMasternodeMan::IsSentinelPingActive()
{
    LOCK(cs);
    // Check if any masternodes have voted recently, otherwise return false
    return (GetTime() - nLastSentinelPingTime) <= Params().GetConsensus().nMasternodeSentinelPingMaxSeconds;
}

bool CMasternodeMan::AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::AddGovernanceVote -- Skip (Masternode not found) \n");
        return false;
    }
    pmn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMasternodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for(auto& mnpair : mapMasternodes) {
        mnpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CMasternodeMan::CheckMasternode(const CPubKey& pubKeyMasternode, bool fForce)
{
    LOCK2(cs_main, cs);
    for (auto& mnpair : mapMasternodes) {
        if (mnpair.second.pubKeyMasternode == pubKeyMasternode) {
            mnpair.second.Check(fForce);
            return;
        }
    }
}

bool CMasternodeMan::IsMasternodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    return pmn ? pmn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CMasternodeMan::SetMasternodeLastPing(const COutPoint& outpoint, const CMasternodePing& mnp)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::SetMasternodeLastPing -- Skip (Masternode not found) \n");
        return;
    }
    pmn->lastPing = mnp;
    if (mnp.fSentinelIsCurrent) {
        UpdateLastSentinelPingTime();
    }
    mapSeenMasternodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CMasternodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mapSeenMasternodeBroadcast.count(hash)) {
        mapSeenMasternodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CMasternodeMan::UpdatedBlockTip(const CBlockIndex *pindex, bool lock)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrintG(BCLogLevel::LOG_NOTICE, BCLog::MN, "[Masternodes] CMasternodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    CheckSameAddr();

    // normal wallet does need to update this every block for mining and block vvalidation, doing update on rpc call would not be enough
    UpdateLastPaid(pindex, lock);
    // if (fMasternodeMode) {
    //     UpdateLastPaid(pindex, lock);
    // }
}
                         
void CMasternodeMan::WarnMasternodeDaemonUpdates()
{
    LOCK(cs);

    static bool fWarned = false;

    if (fWarned)
    {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::WarnMasternodeDaemonUpdates -- Skip (Warned) \n");
        return;
    }

    if (!size())
    {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::WarnMasternodeDaemonUpdates -- Skip (Invalid Size) \n");
        return;
    }

    if (!masternodeSync.IsMasternodeListSynced())
    {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::WarnMasternodeDaemonUpdates -- Skip (Masternode list not synced) \n");
        return;
    }

    int nUpdatedMasternodes{0};

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.lastPing.nDaemonVersion > CLIENT_VERSION) {
            ++nUpdatedMasternodes;
        }
    }

    // Warn only when at least half of known masternodes already updated
    if (nUpdatedMasternodes < size() / 2)
    {
        LogPrintG(BCLogLevel::LOG_INFO, BCLog::MN, "[Masternodes] CMasternodeMan::WarnMasternodeDaemonUpdates -- Skip (Not enough updated masternodes to do a meaningful update) \n");
        return;
    }

    std::string strWarning;
    if (nUpdatedMasternodes != size()) {
        strWarning = strprintf(_("Warning: At least %d of %d masternodes are running on a newer software version. Please check latest releases, you might need to update too."),
                    nUpdatedMasternodes, size());
    } else {
        // someone was postponing this update for way too long probably
        strWarning = strprintf(_("Warning: Every masternode (out of %d known ones) is running on a newer software version. Please check latest releases, it's very likely that you missed a major/critical update."),
                    size());
    }

    // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user
    SetMiscWarning(strWarning);
    // trigger GUI update
    //uiInterface.NotifyAlertChanged(SerializeHash(strWarning), CT_NEW);

    fWarned = true;
}

void CMasternodeMan::NotifyMasternodeUpdates(CConnman& connman)
{
    // Avoid double locking
    bool fMasternodesAddedLocal = false;
    bool fMasternodesRemovedLocal = false;
    {
        LOCK(cs);
        fMasternodesAddedLocal = fMasternodesAdded;
        fMasternodesRemovedLocal = fMasternodesRemoved;
    }

    if (fMasternodesAddedLocal) {
        governance.CheckMasternodeOrphanObjects(connman);
        governance.CheckMasternodeOrphanVotes(connman);
    }
    if (fMasternodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fMasternodesAdded = false;
    fMasternodesRemoved = false;
}
