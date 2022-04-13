// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Crown Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <wallet/wallet.h>

#include <crown/nodewallet.h>
#include <crown/legacysigner.h>
#include <crown/spork.h>
#include <masternode/masternode-budget.h>
#include <masternode/masternode-payments.h>
#include <masternode/masternodeconfig.h>
#include <systemnode/systemnode-payments.h>
#include <pos/stakepointer.h>
#include <pos/stakeminer.h>
#include <pos/stakevalidation.h>


#include <algorithm>
#include <utility>

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    //tx.witness.vtxoutwit.erase(tx.witness.vtxoutwit.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    GenerateCoinbaseCommitment(block, WITH_LOCK(cs_main, return LookupBlockIndex(block.hashPrevBlock)), Params().GetConsensus());

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CTxMemPool& mempool, const CChainParams& params, const Options& options)
    : chainparams(params),
      m_mempool(mempool)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CTxMemPool& mempool, const CChainParams& params)
    : BlockAssembler(mempool, params, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

std::optional<int64_t> BlockAssembler::m_last_block_num_txs{std::nullopt};
std::optional<int64_t> BlockAssembler::m_last_block_weight{std::nullopt};

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, bool fProofOfStake)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK2(cs_main, m_mempool.cs);
    CBlockIndex* pindexPrev = ::ChainActive().Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus());

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    CMutableTransaction txCoinStake;
    if(nHeight >= 1){
        coinbaseTx.nVersion = TX_ELE_VERSION;
        txCoinStake.nVersion = TX_ELE_VERSION;
    }
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;

    if (!fProofOfStake){
        if(Params().NetworkIDString() == CBaseChainParams::TESTNET && nHeight < 1){
            coinbaseTx.vout.resize(1);
            coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
            coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
        }
        else {
            coinbaseTx.vpout.resize(1);
            coinbaseTx.vpout[0].scriptPubKey = scriptPubKeyIn;
            coinbaseTx.vpout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
            coinbaseTx.vpout[0].nAsset = Params().GetConsensus().subsidy_asset;
        }
    }

    if (fProofOfStake && nHeight >= Params().PoSStartHeight()) {
        pblock->nTime = GetAdjustedTime();
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
        bool fStakeFound = false;
        uint32_t nTxNewTime = 0;
        uint32_t nTime = pblock->nTime;
        uint32_t nBits = pblock->nBits;
        StakePointer stakePointer;

        // Slow down blocks so that the testnet chain does not burn through the stakepointers too quick
        if (Params().NetworkIDString() == CBaseChainParams::TESTNET && GetAdjustedTime() - ::ChainActive().Tip()->nTime < 30)
            UninterruptibleSleep(std::chrono::milliseconds(30000));

        if (currentNode.CreateCoinStake(nHeight, nBits, nTime, txCoinStake, nTxNewTime, stakePointer)) {
            pblock->nTime = nTxNewTime;
            txCoinStake.vin[0].scriptSig << nHeight << OP_0;
            pblock->stakePointer = stakePointer;
            fStakeFound = true;
        }

        if (!fStakeFound)
            return NULL;
    }
    // Masternode and general budget payments
    if (IsSporkActive(SPORK_4_ENABLE_MASTERNODE_PAYMENTS) || Params().NetworkIDString() == CBaseChainParams::TESTNET) {
        FillBlockPayee(coinbaseTx, nFees);
        SNFillBlockPayee(coinbaseTx, nFees);
    }
    // Proof of stake blocks pay the mining reward in the coinstake transaction
    if (fProofOfStake) {
        CAmount nValueNodeRewards = 0;
        if(coinbaseTx.nVersion >= TX_ELE_VERSION){
            if (coinbaseTx.vpout.size() > 1)
                nValueNodeRewards += coinbaseTx.vpout[MN_PMT_SLOT].nValue;
            if (coinbaseTx.vpout.size() > 2)
                nValueNodeRewards += coinbaseTx.vpout[SN_PMT_SLOT].nValue;
        }
        else {
            if (coinbaseTx.vout.size() > 1)
                nValueNodeRewards += coinbaseTx.vout[MN_PMT_SLOT].nValue;
            if (coinbaseTx.vout.size() > 2)
                nValueNodeRewards += coinbaseTx.vout[SN_PMT_SLOT].nValue;
        }

        if (!(IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(pindexPrev->nHeight + 1))) {
            //Reduce PoS reward by the node rewards
            if(txCoinStake.nVersion >= TX_ELE_VERSION)
                txCoinStake.vpout[0].nValue = GetBlockValue(nHeight, nFees) - nValueNodeRewards;
            else
                txCoinStake.vout[0].nValue = GetBlockValue(nHeight, nFees) - nValueNodeRewards;
        } else {
            // Miner gets full block value in case of superblock
            if(txCoinStake.nVersion >= TX_ELE_VERSION)
                txCoinStake.vpout[0].nValue = GetBlockValue(nHeight, nFees);
            else
                txCoinStake.vout[0].nValue = GetBlockValue(nHeight, nFees);
        }

        // Make sure coinbase has null values
        if(txCoinStake.nVersion >= TX_ELE_VERSION){
            coinbaseTx.vpout[0].scriptPubKey = CScript();
            coinbaseTx.vpout[0].nValue = 0;
            coinbaseTx.vpout[0].nAsset = Params().GetConsensus().subsidy_asset;
        }
        else {
            coinbaseTx.vout[0].scriptPubKey = CScript();
            coinbaseTx.vout[0].nValue = 0;
        }
    }

    if (!(IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(nHeight))) {

        if(txCoinStake.nVersion >= TX_ELE_VERSION){
            // Make payee
            if(coinbaseTx.vpout.size() > 1)
                pblock->payee = coinbaseTx.vpout[MN_PMT_SLOT].scriptPubKey;
            // Make SNpayee
            if(coinbaseTx.vpout.size() > 2)
               pblock->payeeSN = coinbaseTx.vpout[SN_PMT_SLOT].scriptPubKey;
        }
        else {
            // Make payee
            if(coinbaseTx.vout.size() > 1)
                pblock->payee = coinbaseTx.vout[MN_PMT_SLOT].scriptPubKey;
            // Make SNpayee
            if(coinbaseTx.vout.size() > 2)
               pblock->payeeSN = coinbaseTx.vout[SN_PMT_SLOT].scriptPubKey;
        }
    }
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    if (fProofOfStake){
        pblock->vtx.emplace_back();
        pblock->vtx[1] = MakeTransactionRef(std::move(txCoinStake));
    }

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
    pblocktemplate->vTxFees[0] = -nFees;
    
    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (!fProofOfStake) {
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    }
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    // Sign Block
    if (fProofOfStake) {
        pblock->SetProofOfStake(true);
        if (!SignBlock(pblock)) {
            LogPrintf("%s: Failed to sign block\n", __func__);
            return NULL;
        }
    }

    BlockValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms) \n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        m_mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert(it != m_mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = m_mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != m_mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != m_mempool.mapTx.get<ancestor_score>().end() &&
            SkipMapTxEntry(m_mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == m_mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = m_mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        m_mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce));
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

// stake minter thread
void ThreadStakeMiner(CWallet *pwallet)
{
    LogPrintf("ThreadStakeMiner started\n");
    unsigned int nExtraNonce = 0;
    const CTxMemPool& mempool = pwallet->chain().getMempool();

    CScript dummyscript;

    bool fTryToSync = true;
    UninterruptibleSleep(std::chrono::seconds{180});

    try
    {
        while (true)
        {
            while (pwallet->IsLocked())
            {
                LogPrintf("%s : Not staking Wallet Locked \n", __func__);
                UninterruptibleSleep(std::chrono::seconds{60});
            }

            if (::ChainActive().Height() + 1 < Params().PoSStartHeight() || (!fMasterNode && !fSystemNode) ||
                ::ChainActive().Tip()->GetBlockTime() > GetAdjustedTime())
            {
                // 1. If the height is not reached to POS height
                // 2. If it is neither a masternode nor a systemnode
                // 3. If the block time is bigger then adjusted time
                UninterruptibleSleep(std::chrono::seconds{10});
                continue;
            }
            else
            {
                //Check the state of the blockchain being synced before trying to stake
                if (!masternodeSync.IsBlockchainSynced()) {
                    if (!gArgs.GetBoolArg("-jumpstart", false)) {
                        UninterruptibleSleep(std::chrono::seconds{10});
                        continue;
                    }
                }
            }
            //
            // Create new block
            //
            //LOCK(pwallet->cs_wallet);
            if(pwallet->HaveAvailableCoinsForStaking())
            {
                // First just create an empty block. No need to process transactions until we know we can create a block
                std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(mempool, Params()).CreateNewBlock(dummyscript, pwallet, true));

                if (!pblocktemplate.get())
                {
                    LogPrintf("Error in ThreadStakeMiner \n");
                    continue;
                    //return;
                }
                CBlock *pblock = &pblocktemplate->block;
                //IncrementExtraNonce(pblock, ::ChainActive().Tip(), nExtraNonce);

                // if proof-of-stake block found then process block
                // Process this block the same as if we had received it from another node
                std::shared_ptr<CBlock> shared_pblock = std::make_shared<CBlock>(*pblock);

                if (!g_chainman.ProcessNewBlock(Params(), shared_pblock, true, nullptr)){
                    LogPrintf("ThreadStakeMiner : ProcessBlock, block not accepted \n");
                    return;
                }
            }
            else {
                LogPrintf("%s: no available coins\n", __func__);
                UninterruptibleSleep(std::chrono::seconds{600});
            }
        }
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("ThreadStakeMiner runtime error: %s\n", e.what());
        return;
    }
    catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMiner()");
    } catch (...) {
        PrintExceptionContinue(nullptr, "ThreadStakeMiner()");
    }
    LogPrintf("ThreadStakeMiner exiting\n");
}

void Stake(bool fStake, CWallet *pwallet, std::thread* stakeThread)
{
    if (stakeThread != nullptr)
    {
        if (stakeThread->joinable())
            stakeThread->join();
        stakeThread = nullptr;
    }
    if(fStake)
    {
        stakeThread = new std::thread([&, pwallet] { TraceThread("stake", [&, pwallet] { ThreadStakeMiner(pwallet); }); });
    }
}
