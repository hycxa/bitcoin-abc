// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validation.h"

#include "arith_uint256.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "init.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "script/scriptcache.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "timedata.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"
#include "warnings.h"

#include <atomic>
#include <sstream>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/thread.hpp>

#if defined(NDEBUG)
#error "Bitcoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

CCriticalSection cs_main;

BlockMap mapBlockIndex;
CChain chainActive;
CBlockIndex *pindexBestHeader = nullptr;  //标识当前激活链的Tip块
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
std::atomic_bool fImporting(false);
bool fReindex = false;
bool fTxIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

uint256 hashAssumeValid;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
Amount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

CTxMemPool mempool(::minRelayTxFee);

static void CheckBlockIndex(const Consensus::Params &consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "Bitcoin Signed Message:\n";

// Internal stuff
namespace {

struct CBlockIndexWorkComparator {
    bool operator()(CBlockIndex *pa, CBlockIndex *pb) const {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex *pindexBestInvalid;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself
 * and all ancestors) and as good as our current tip or better. Entries may be
 * failed, though, and pruning nodes may be missing the data for the block.
 * 该集合中存储的所有区块状态(并且要求该区块的祖先状态也要)都为BLOCK_VALID_TRANSACTIONS；并且
 * 这些区块的工作量大于当前 激活链的 Tip块。  功能就是：作为Tip 的候选区块，因为分叉问题，可能会有多个候选块
 */
std::set<CBlockIndex *, CBlockIndexWorkComparator> setBlockIndexCandidates;
/**
 * All pairs A->B, where A (or one of its ancestors) misses transactions, but B
 * has transactions. Pruned nodes may have entries where B is missing data. 修剪的节点含有的条目B，可能缺少数据
 * A 为丢失的祖先交易， B为当前检查的交易
 */
std::multimap<CBlockIndex *, CBlockIndex *> mapBlocksUnlinked;

CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;     //最后一个文件的序号；
/**
 * Global flag to indicate we should check to see if there are block/undo files
 * that should be deleted. Set on startup or if we allocate more file space when
 * we're in prune mode.
 * 全局变量：检查是否block/undo文件是否应该被删除。可以在启动设置，或者在裁剪模式下可以分配的更多的文件空间
 */
bool fCheckForPruning = false;

/**
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
CCriticalSection cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
int32_t nBlockSequenceId = 1;
/** Decreasing counter (used by subsequent preciousblock calls).
 * 降低计数(用于 以后的 preciousblock 调用)
 * */
int32_t nBlockReverseSequenceId = -1;
/** chainwork for the last block that preciousblock has been applied to.
 * 最近的preciousblock 的链工作量
 * */
arith_uint256 nLastPreciousChainwork = 0;

/** Dirty block index entries. 脏块的索引条目 */
std::set<CBlockIndex *> setDirtyBlockIndex;

/** Dirty block file entries 存储脏文件条目. */
std::set<int> setDirtyFileInfo;
} // namespace

/* Use this class to start tracking transactions that are removed from the
 * mempool and pass all those transactions through SyncTransaction when the
 * object goes out of scope. This is currently only used to call SyncTransaction
 * on conflicts removed from the mempool during block connection.  Applied in
 * ActivateBestChain around ActivateBestStep which in turn calls:
 * ConnectTip->removeForBlock->removeConflicts
 * 使用这个类跟踪从mempool中移除的交易，并且当对象生命周期结束时，使用SyncTransaction 通知所有的订阅这个事件的
 * 对方，该交易被移除了。这是当前唯一在区块接收期间，从mempool移除交易，产生冲突时调用SyncTransaction 的方法。
 * 在ActivateBestChain 和 ActivateBestStep 之间使用
 */
class MemPoolConflictRemovalTracker {
private:
    std::vector<CTransactionRef> conflictedTxs;     //
    CTxMemPool &pool;

public:
    //    boost::signals2::signal<void(CTransactionRef, MemPoolRemovalReason)>
    //    NotifyEntryRemoved;     //移除交易的信号； 参一：操作的交易； 参二：操作的原因
    MemPoolConflictRemovalTracker(CTxMemPool &_pool) : pool(_pool) {
        pool.NotifyEntryRemoved.connect(boost::bind(
            &MemPoolConflictRemovalTracker::NotifyEntryRemoved, this, _1, _2));
    }

    void NotifyEntryRemoved(CTransactionRef txRemoved,
                            MemPoolRemovalReason reason) {
        if (reason == MemPoolRemovalReason::CONFLICT) {
            conflictedTxs.push_back(txRemoved);
        }
    }

    ~MemPoolConflictRemovalTracker() {
        pool.NotifyEntryRemoved.disconnect(boost::bind(
            &MemPoolConflictRemovalTracker::NotifyEntryRemoved, this, _1, _2));
        for (const auto &tx : conflictedTxs) {
            GetMainSignals().SyncTransaction(
                *tx, nullptr, CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);
        }
        conflictedTxs.clear();
    }
};

CBlockIndex *FindForkInGlobalIndex(const CChain &chain,
                                   const CBlockLocator &locator) {
    // Find the first block the caller has in the main chain
    for (const uint256 &hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex *pindex = (*mi).second;
            if (chain.Contains(pindex)) return pindex;
            if (pindex->GetAncestor(chain.Height()) == chain.Tip()) {
                return chain.Tip();
            }
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = nullptr;
CBlockTreeDB *pblocktree = nullptr;

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

// See definition for documentation
static bool FlushStateToDisk(CValidationState &state, FlushStateMode mode,
                             int nManualPruneHeight = 0);
static void FindFilesToPruneManual(std::set<int> &setFilesToPrune,
                                   int nManualPruneHeight);
static uint32_t GetBlockScriptFlags(const CBlockIndex *pindex,
                                    const Config &config);

static bool IsFinalTx(const CTransaction &tx, int nBlockHeight,
                      int64_t nBlockTime) {
    if (tx.nLockTime == 0) {
        return true;
    }

    int64_t lockTime = tx.nLockTime;
    int64_t lockTimeLimit =
        (lockTime < LOCKTIME_THRESHOLD) ? nBlockHeight : nBlockTime;
    if (lockTime < lockTimeLimit) {
        return true;
    }

    for (const auto &txin : tx.vin) {
        if (txin.nSequence != CTxIn::SEQUENCE_FINAL) {
            return false;
        }
    }
    return true;
}

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 * tx(in):检查的交易； flags(in):交易检查的标识； prevHeights(in/out):这个交易的所有引用输出所在的高度, 当这个
 * 交易输入的sequence字段不表示相对锁定时间时，将指定的交易输入的高度设为0。
 * block(in):这个交易将在这个块中被打包(假设)。
 */
static std::pair<int, int64_t>
CalculateSequenceLocks(const CTransaction &tx, int flags,
                       std::vector<int> *prevHeights,
                       const CBlockIndex &block) {

    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    // 判断该交易是否支持 BIP68
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2 &&(
                         flags & LOCKTIME_VERIFY_SEQUENCE != 0);

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to; 不支持BIP68，
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    // 遍历该交易的所有交易输入
    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn &txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        // 如果交易输入的sequence字段 设置了SEQUENCE_LOCKTIME_DISABLE_FLAG，
        // 标识这个字段将不做为这个交易输入的对应的锁定时间(详细信息：查看BIP68)
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        //获取这个交易输入的高度
        int nCoinHeight = (*prevHeights)[txinIndex];
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            //这个flag被设置，标识sequence字段是以时间进行锁定的。

            //以假设的打包这个交易的块为基点，查找引用交易输入的父交易的中值时间。
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight - 1, 0))
                                    ->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics.
            // BIP 68 relative lock times have the semantics of calculating the
            // first block or time at which the transaction would be valid. When
            // calculating the effective block time or height for the entire
            // transaction, we switch to using the semantics of nLockTime which
            // is the last invalid block time or height. Thus we subtract 1 from
            // the calculated time or height.

            // Time-based relative lock-times are measured from the smallest
            // allowed timestamp of the block containing the txout being spent,
            // which is the median time past of the block prior.
            // 计算这个最低的时间
            nMinTime = std::max(
                nMinTime,
                nCoinTime +
                    (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK)
                              << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) -
                    1);
        } else {
            //标识sequence字段是以高度作为锁定的
            nMinHeight = std::max(
                nMinHeight,
                nCoinHeight +
                    (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    //返回求得的该交易的所有的交易输入中最迟的可以花费的时间 和 高度(即什么时候这个交易的所有输入 才都可以花费，这个交易此时才可以打包进区块)
    //因为一个交易可能含有多个交易输入， 每个交易输入可能采用了不同的锁定方式(时间和高度)，所以返回这个交易中 此2者的最大值。只要最大值检查通过后，
    //就表示这个交易都可以被打包了。
    return std::make_pair(nMinHeight, nMinTime);
}

//执行锁定时间的检查；
static bool EvaluateSequenceLocks(const CBlockIndex &block,
                                  std::pair<int, int64_t> lockPair) {
    assert(block.pprev);
    //获取这个交易的中值时间，比较
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags,
                   std::vector<int> *prevHeights, const CBlockIndex &block) {
    return EvaluateSequenceLocks(
        block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

bool TestLockPointValidity(const LockPoints *lp) {
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the
    // chain
    // 如果有相对锁定时间，并且maxInputBlock 含有数据；如果每天相对锁定时间，LockPoints 不依赖于当前的块链。
    if (lp->maxInputBlock) {
        // Check whether chainActive is an extension of the block at which the
        // LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        // 计算当前链是否包含该锁定点
        if (!chainActive.Contains(lp->maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

//检查交易的时间锁；通过交易输入的sequence字段
//tx(in):检查的交易； flags(in):检查该交易的flag； lp(out):为该交易创建锁定点；
//useExistingLockPoints(in): 默认为false，该值标识，参三是否为已含有数据的 LockPoints.
bool CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints *lp,
                        bool useExistingLockPoints) {
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    //1. 获取当前链的高度；
    CBlockIndex *tip = chainActive.Tip();
    // 创建当前链的下一个块的索引；并设置它的属性；父索引和高度
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate height based
    // locks because when SequenceLocks() is called within ConnectBlock(), the
    // height of the block *being* evaluated is what is used. Thus if we want to
    // know if a transaction can be part of the *next* block, we need to use one
    // more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    //2. 如果参三已含有数据
    if (useExistingLockPoints) {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    } else {
        //3. 未含有数据，需要给参三加入数据。
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        // 创建一个包含 UTXO集合和mempool的 视角
        CCoinsViewMemPool viewMemPool(pcoinsTip, mempool);

        std::vector<int> prevheights;       //这个变量存储该交易的引用输出的交易的高度(即它所花费的UTXO的高度)；
        // 这个UTXO可以是UTXO集合中的，也可以是mempool中未打包的交易，(注意：所有mempool中的未打包的交易充当UTXO时，高度都设置为 MEMPOOL_HEIGHT)
        prevheights.resize(tx.vin.size());
        //  遍历该交易的所有交易输入
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {

            const CTxIn &txin = tx.vin[txinIndex];
            Coin coin;
            // 在创建的视角中查找 该引用输出的UTXO。
            if (!viewMemPool.GetCoin(txin.prevout, coin)) {
                return error("%s: Missing input", __func__);
            }
            // 如果查找的UTXO是在 交易池中，即该交易依赖于一个还未打包的交易； 所有交易池的UTXO的高度都为  MEMPOOL_HEIGHT。
            if (coin.GetHeight() == MEMPOOL_HEIGHT) {
                // Assume all mempool transaction confirm in the next block；
                // 假设mempool中的所有交易都会在下一个块中被打包。
                prevheights[txinIndex] = tip->nHeight + 1;
            } else {
                // 该UTXO存在于UTXO集合中；则获取该UTXO的高度
                prevheights[txinIndex] = coin.GetHeight();
            }
        }
        // 计算 该交易的锁定时间戳
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp) {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of all
            // the blocks which have sequence locked prevouts. This hash needs
            // to still be on the chain for these LockPoint calculations to be
            // valid.
            // Note: It is impossible to correctly calculate a maxInputBlock if
            // any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height is
            // 0, which is equivalent to no sequence lock. Since we assume input
            // height of tip+1 for mempool txs and test the resulting lockPair
            // from CalculateSequenceLocks against tip+1. We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            // 查找最大的 引用交易输入的 高度；
            int maxInputHeight = 0;

            for (int height : prevheights) {
                // Can ignore mempool inputs since we'll fail if they had
                // non-zero locks
                if (height != tip->nHeight + 1) {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            // 获取该交易的最大的交易输入 块索引。
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    // 执行
    return EvaluateSequenceLocks(index, lockPair);
}

uint64_t GetSigOpCountWithoutP2SH(const CTransaction &tx) {
    uint64_t nSigOps = 0;
    for (const auto &txin : tx.vin) {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto &txout : tx.vout) {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

uint64_t GetP2SHSigOpCount(const CTransaction &tx,
                           const CCoinsViewCache &inputs) {
    if (tx.IsCoinBase()) {
        return 0;
    }

    uint64_t nSigOps = 0;
    for (auto &i : tx.vin) {
        const CTxOut &prevout = inputs.GetOutputFor(i);
        if (prevout.scriptPubKey.IsPayToScriptHash()) {
            nSigOps += prevout.scriptPubKey.GetSigOpCount(i.scriptSig);
        }
    }
    return nSigOps;
}

uint64_t GetTransactionSigOpCount(const CTransaction &tx,
                                  const CCoinsViewCache &inputs, int flags) {
    uint64_t nSigOps = GetSigOpCountWithoutP2SH(tx);
    if (tx.IsCoinBase()) {
        return nSigOps;
    }

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs);
    }

    return nSigOps;
}

//检查交易的公共部分
static bool CheckTransactionCommon(const CTransaction &tx,
                                   CValidationState &state,
                                   bool fCheckDuplicateInputs) {
    // Basic checks that don't depend on any context
    if (tx.vin.empty()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    }

    if (tx.vout.empty()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    }

    // Size limit
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_TX_SIZE) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");
    }

    // Check for negative or overflow output values
    Amount nValueOut = 0;
    for (const auto &txout : tx.vout) {
        if (txout.nValue < 0) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-vout-negative");
        }

        if (txout.nValue > MAX_MONEY) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-vout-toolarge");
        }

        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-txouttotal-toolarge");
        }
    }

    if (GetSigOpCountWithoutP2SH(tx) > MAX_TX_SIGOPS_COUNT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txn-sigops");
    }

    // Check for duplicate inputs - note that this check is slow so we skip it
    // in CheckBlock
    if (fCheckDuplicateInputs) {
        std::set<COutPoint> vInOutPoints;
        for (const auto &txin : tx.vin) {
            if (!vInOutPoints.insert(txin.prevout).second) {
                return state.DoS(100, false, REJECT_INVALID,
                                 "bad-txns-inputs-duplicate");
            }
        }
    }

    return true;
}

bool CheckCoinbase(const CTransaction &tx, CValidationState &state,
                   bool fCheckDuplicateInputs) {
    if (!tx.IsCoinBase()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false,
                         "first tx is not coinbase");
    }

    if (!CheckTransactionCommon(tx, state, fCheckDuplicateInputs)) {
        // CheckTransactionCommon fill in the state.
        return false;
    }

    if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
    }

    return true;
}

//检查普通的的交易；
bool CheckRegularTransaction(const CTransaction &tx, CValidationState &state,
                             bool fCheckDuplicateInputs) {
    if (tx.IsCoinBase()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-tx-coinbase");
    }

    if (!CheckTransactionCommon(tx, state, fCheckDuplicateInputs)) {
        // CheckTransactionCommon fill in the state.
        return false;
    }

    for (const auto &txin : tx.vin) {
        if (txin.prevout.IsNull()) {
            return state.DoS(10, false, REJECT_INVALID,
                             "bad-txns-prevout-null");
        }
    }

    return true;
}

void LimitMempoolSize(CTxMemPool &pool, size_t limit, unsigned long age) {
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0) {
        LogPrint("mempool", "Expired %i transactions from the memory pool\n",
                 expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint &removed : vNoSpendsRemaining) {
        pcoinsTip->Uncache(removed);
    }
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state) {
    return strprintf(
        "%s%s (code %i)", state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage(),
        state.GetRejectCode());
}

//标识当前主链是否可以进行交易预估(当主链太过于落后时，不应该拿来估算交易费)
static bool IsCurrentForFeeEstimation() {
    AssertLockHeld(cs_main);
    if (IsInitialBlockDownload()) {
        return false;
    }
    //主链的最大时间，小于当前时间减去最大时间差；标识当前主链应该太过于落后
    if (chainActive.Tip()->GetBlockTime() <
        (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE)) {
        return false;
    }
    //标识当前主链应该太过于落后
    if (chainActive.Height() < pindexBestHeader->nHeight - 1) {
        return false;
    }
    return true;
}

static bool IsUAHFenabled(const Config &config, int nHeight) {
    return nHeight >= config.GetChainParams().GetConsensus().uahfHeight;
}

bool IsUAHFenabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsUAHFenabled(config, pindexPrev->nHeight);
}

static bool IsCashHFEnabled(const Config &config, int64_t nMedianTimePast) {
    return nMedianTimePast >=
           config.GetChainParams().GetConsensus().cashHardForkActivationTime;
}

bool IsCashHFEnabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsCashHFEnabled(config, pindexPrev->GetMedianTimePast());
}

// Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
// were somehow broken and returning the wrong scriptPubKeys
//
static bool CheckInputsFromMempoolAndCache(const CTransaction &tx,
                                           CValidationState &state,
                                           const CCoinsViewCache &view,
                                           CTxMemPool &pool, uint32_t flags,
                                           bool cacheSigStore,
                                           PrecomputedTransactionData &txdata) {
    AssertLockHeld(cs_main);

    // pool.cs should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view and
    // when we actually call through to CheckInputs
    LOCK(pool.cs);

    //mempool 中不含有coinbase交易
    assert(!tx.IsCoinBase());
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);

        // At this point we haven't actually checked if the coins are all
        // available (or shouldn't assume we have, since CheckInputs does). So
        // we just return failure if the inputs are not available here, and then
        // only have to check equivalence for available inputs.
        if (coin.IsSpent()) {
            return false;
        }

        const CTransactionRef &txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->vout.size() > txin.prevout.n);
            assert(txFrom->vout[txin.prevout.n] == coin.GetTxOut());
        } else {
            const Coin &coinFromDisk = pcoinsTip->AccessCoin(txin.prevout);
            assert(!coinFromDisk.IsSpent());
            assert(coinFromDisk.GetTxOut() == coin.GetTxOut());
        }
    }

    return CheckInputs(tx, state, view, true, flags, cacheSigStore, true,
                       txdata);
}

//向交易池中添加一个交易
//接收交易池工作者； pool(in)：交易池; state(out):状态；ptx(in):交易
//pfMissingInputs(out):标识该笔交易有未找到的交易输入存在；即引用输出不存在于本节点的UTXO集合中
static bool AcceptToMemoryPoolWorker(
    const Config &config, CTxMemPool &pool, CValidationState &state,
    const CTransactionRef &ptx, bool fLimitFree, bool *pfMissingInputs,
    int64_t nAcceptTime, std::list<CTransactionRef> *plTxnReplaced,
    bool fOverrideMempoolLimit, const Amount nAbsurdFee,
    std::vector<COutPoint> &coins_to_uncache) {

    AssertLockHeld(cs_main);

    const CTransaction &tx = *ptx;
    const uint256 txid = tx.GetId();
    if (pfMissingInputs) {
        *pfMissingInputs = false;
    }

    //1. Coinbase is only valid in a block, not as a loose transaction.； 检查普通的交易
    if (!CheckRegularTransaction(tx, state, true)) {
        // state filled in by CheckRegularTransaction.
        return false;
    }

    //2. Rather not work on nonstandard transactions (unless -testnet/-regtest)
    // 继续进行标准交易的检查
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason)) {
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);
    }

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    //3. 只接受可以打包的交易，还是进行交易的检查
    CValidationState ctxState;
    if (!ContextualCheckTransactionForCurrentBlock(
            config, tx, ctxState, config.GetChainParams().GetConsensus(),
            STANDARD_LOCKTIME_VERIFY_FLAGS)) {
        // We copy the state from a dummy to ensure we don't increase the
        // ban score of peer for transaction that could be valid in the future.
        return state.DoS(
            0, false, REJECT_NONSTANDARD, ctxState.GetRejectReason(),
            ctxState.CorruptionPossible(), ctxState.GetDebugMessage());
    }

    // Is it already in the memory pool? 交易池已存在该交易。
    //4. 查看该交易是否已在交易池中
    if (pool.exists(txid)) {
        return state.Invalid(false, REJECT_ALREADY_KNOWN,
                             "txn-already-in-mempool");
    }

    // Check for conflicts with in-memory transactions； 检查交易池中的交易冲突
    {
        // Protect pool.mapNextTx
        LOCK(pool.cs);
        //即该交易的引用输出已被交易池中的其它交易使用；
        // 5. 查看该交易是否与交易池中的其他交易 使用了相同的引用输出。
        for (const CTxIn &txin : tx.vin) {
            auto itConflicting = pool.mapNextTx.find(txin.prevout);
            if (itConflicting != pool.mapNextTx.end()) {
                // Disable replacement feature for good
                return state.Invalid(false, REJECT_CONFLICT,
                                     "txn-mempool-conflict");
            }
        }
    }

    {

        CCoinsView dummy;       //将临时变量作为UTXO的后端，
        CCoinsViewCache view(&dummy);

        Amount nValueIn = 0;
        LockPoints lp;
        {
            LOCK(pool.cs);
            //使用 UTXO和mempool 的全局状态构建一个查找器;作为UTXO的后端；
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            // 将这个变量设为 view的后端，此时，直接后端链接到数据库
            view.SetBackend(viewMemPool);
            //6. 查看该交易是否已被确认 或 已进入交易池
            // Do we already have it? 查找UTXO集合是否已存在该交易；存在，标识该交易已被确认，不需要再次进入交易池。
            for (size_t out = 0; out < tx.vout.size(); out++) {
                //1. 构建以该交易作为输出的 preout.
                COutPoint outpoint(txid, out);

                //查看该交易的输出是否已在 UTXO集合中，
                bool had_coin_in_cache = pcoinsTip->HaveCoinInCache(outpoint);      //查看UTXO缓存集合中是否存在该UTXO
                // 查看该交易的某个输出 是否已存在UTXO或交易池中。存在，进入下列条件
                if (view.HaveCoin(outpoint)) {
                    // 不存在与UTXO集合中，存在于交易池中
                    if (!had_coin_in_cache) {
                        //如果该UTXO不存在于UTXO集合中，但是存在于交易池中，将它添加到 传出变量中。
                        coins_to_uncache.push_back(outpoint);
                    }
                    // 标识该交易已经被确认(存在于UTXO集合中) 或 已在交易池中被其他交易引用(即：已存在与交易池中)
                    return state.Invalid(false, REJECT_ALREADY_KNOWN,
                                         "txn-already-known");
                }
            }

            // 运行到此处：标识该交易既没有在UTXO中(属于未确认的交易)，也不在交易池中;(此时可以确定：该交易属于新交易)
            // Do all inputs exist?； 查看该交易的交易输入在 UTXO集合中是否都存在
            //7. 查看该交易的引用输出是否都存在；即它花费的UTXO是否合理
            for (const CTxIn txin : tx.vin) {
                //1. 它的引用输出不存在与UTXO集合中，将它添加到 传出变量中
                if (!pcoinsTip->HaveCoinInCache(txin.prevout)) {
                    coins_to_uncache.push_back(txin.prevout);
                }
                //2. 如果UTXO集合 与 交易池中 都不存在该引用输出。
                if (!view.HaveCoin(txin.prevout)) {
                    // 对传出参数赋值，标识该笔交易有未找到的交易输入存在；即引用输出不存在与本节点的UTXO集合中
                    if (pfMissingInputs) {
                        *pfMissingInputs = true;
                    }

                    // fMissingInputs and !state.IsInvalid() is used to detect
                    // this condition, don't set state.Invalid()  直接返回出错
                    return false;
                }
            }

            //8. 查看该交易所有的 引用输出是否都存在；
            // Are the actual inputs available?; 查看该交易的所有交易输入的引用输出是否都可以在UTXO集合中找到
            if (!view.HaveInputs(tx)) {
                return state.Invalid(false, REJECT_DUPLICATE,
                                     "bad-txns-inputs-spent");
            }

            // Bring the best block into scope. 将最好的块带入范围
            view.GetBestBlock();

            //获取该交易的所有 交易输入金额
            nValueIn = view.GetValueIn(tx); //返回一个交易在本节点的UTXO集合中的输入总金额，

            // We have all inputs cached now, so switch back to dummy, so we
            // don't need to keep lock on mempool.
            //切换后端，不要一直锁定交易池； 此时刚才查找的所有交易，都已写入临时view的缓存中
            view.SetBackend(dummy);

            // Only accept BIP68 sequence locked transactions that can be mined
            // in the next block; we don't want our mempool filled up with
            // transactions that can't be mined yet. Must keep pool.cs for this
            // unless we change CheckSequenceLocks to take a CoinsViewCache
            // instead of create its own；.
            // 检查时间锁; 并为这个交易创建 lockpoints.
            if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp)) {
                return state.DoS(0, false, REJECT_NONSTANDARD,
                                 "non-BIP68-final");
            }
        }

        //9. 继续检查该交易
        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view)) {
            return state.Invalid(false, REJECT_NONSTANDARD,
                                 "bad-txns-nonstandard-inputs");
        }

        //10. 获取签名的操作数量。
        int64_t nSigOpsCount =
            GetTransactionSigOpCount(tx, view, STANDARD_SCRIPT_VERIFY_FLAGS);

        //11. 返回交易输出的总金额，计算交易费
        Amount nValueOut = tx.GetValueOut();
        Amount nFees = nValueIn - nValueOut;
        // nModifiedFees includes any fee deltas from PrioritiseTransaction;
        // nModifiedFees 包含所有来自PrioritiseTransaction的交易费率
        Amount nModifiedFees = nFees;
        double nPriorityDummy = 0;
        //如果该交易在交易池中找到，则参二和参三已被更新状态
        pool.ApplyDeltas(txid, nPriorityDummy, nModifiedFees);

        Amount inChainInputValue;
        //计算交易的优先级，同时获取交易的输入总金额(从UTXO集合中获取的)
        double dPriority =
            view.GetPriority(tx, chainActive.Height(), inChainInputValue);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        //持续跟踪一个花费coinbase的交易，在块链重组期间，需要重新扫描，保证该coinbase交易始终处于可花费的状态。
        bool fSpendsCoinbase = false;
        //12. 查看该交易中，是否花费了一个coinbase交易，如果花费了，更新bool状态
        for (const CTxIn &txin : tx.vin) {
            const Coin &coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }
        //创建一个mempoolEntry;   交易；交易费；接收时间；交易的优先级；接收到该交易时主链的高度；交易的输入总金额；是否花费了一个coinbase交易；签名数量；
        CTxMemPoolEntry entry(ptx, nFees.GetSatoshis(), nAcceptTime, dPriority,
                              chainActive.Height(),
                              inChainInputValue.GetSatoshis(), fSpendsCoinbase,
                              nSigOpsCount, lp);
        //获取交易的序列化大小
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS_PER_MB; we still consider this an invalid rather
        // than merely non-standard transaction.
        //检查该交易没有超出签名数量的限制，保证该交易可以被打包。 因为coinbase交易可以包含MAX_STANDARD_TX_SIGOPS签名操作码；
        //但是仍然认为这个非标准交易是无效的
        if (nSigOpsCount > MAX_STANDARD_TX_SIGOPS) {
            return state.DoS(0, false, REJECT_NONSTANDARD,
                             "bad-txns-too-many-sigops", false,
                             strprintf("%d", nSigOpsCount));
        }

        //12. 返回该交易在矿池最低的进入费率时应给的 手续费。 (即：该交易这些字节数进入矿池时，应该给的最低交易费)
        Amount mempoolRejectFee =
            pool.GetMinFee(GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) *
                           1000000)
                .GetFee(nSize)
                .GetSatoshis();
        // 手续费太低，不让该交易进入交易池
        if (mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                             "mempool min fee not met", false,
                             strprintf("%d < %d", nFees, mempoolRejectFee));
        }
        //中继策略； 当手续费低于中继费用时，且交易优先级不到免费中继时，仍然不让进入交易池。
        if (GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) &&
            nModifiedFees < ::minRelayTxFee.GetFee(nSize) &&
            !AllowFree(entry.GetPriority(chainActive.Height() + 1))) {
            // Require that free transactions have sufficient priority to be
            // mined in the next block.
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                             "insufficient priority");
        }

        // Continuously rate-limit free (really, very-low-fee) transactions.
        // This mitigates 'penny-flooding' -- sending thousands of free
        // transactions just to be annoying or make others' transactions take
        // longer to confirm.
        //13. 持续限制免交易费的交易速度。这将减少大量低交易费。不符合条件，同样不允许进入交易池。
        if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize)) {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window: 使用一个成倍衰减的窗口(每10分钟)
            dFreeCount *= pow(1.0 - 1.0 / 600.0, double(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            // 限制免费中继的交易是：每分钟千字节。在默认速率下，它将一个多月才可能传输超过1GB。传输超过限制，也不进行中继
            if (dFreeCount + nSize >=
                GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) * 10 * 1000) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                                 "rate limited free transaction");
            }

            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount,
                     dFreeCount + nSize);
            //累加
            dFreeCount += nSize;
        }

        //14. 当交易费大于 不合理的交易费时，也不进行中继(因为可能误操作，导致很高的交易费，忘记找零)
        if (nAbsurdFee != 0 && nFees > nAbsurdFee) {
            return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee",
                                 strprintf("%d > %d", nFees, nAbsurdFee));
        }

        // Calculate in-mempool ancestors, up to a limit.  计算该交易在交易池中的祖先；获取一个交易祖先的限制属性
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors =
            GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize =
            GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants =
            GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize =
            GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) *
            1000;
        std::string errString;
        //15. 计算该交易在交易池中的所有祖先交易
        if (!pool.CalculateMemPoolAncestors(
                entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, false, REJECT_NONSTANDARD,
                             "too-long-mempool-chain", false, errString);
        }

        //获取验证标识
        uint32_t scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (!Params().RequireStandard()) {
            scriptVerifyFlags =
                GetArg("-promiscuousmempoolflags", scriptVerifyFlags);
        }

        // Check against previous transactions. This is done last to help
        // prevent CPU exhaustion denial-of-service attacks.
        //检查以前的交易。可以保护CPU免遭拒绝服务攻击。
        PrecomputedTransactionData txdata(tx);
        //16. 检查所有的交易输入， 不符合，同样不允许进入交易池
        if (!CheckInputs(tx, state, view, true, scriptVerifyFlags, true, false,
                         txdata)) {
            // State filled in by CheckInputs.
            return false;
        }

        // Check again against the current block tip's script verification flags
        // to cache our script execution flags. This is, of course, useless if
        // the next block has different script flags from the previous one, but
        // because the cache tracks script flags for us it will auto-invalidate
        // and we'll just have a few blocks of extra misses on soft-fork
        // activation.
        // 再次检查当前链的Tip脚本验证标识，并更新我们的脚本验证标识。 当然，如果接下来的块与以
        // 前的块标识不同，将会失败。但是缓存这个标识可以自动废止，仅有额外很少的块需要没有使用软分叉激活。
        //
        // This is also useful in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain CHECKSIG
        // NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks (using TestBlockValidity), however allowing such
        // transactions into the mempool can be exploited as a DoS attack.
        // 在 CreateNewBlock一个新块中也有一个类似的检查，来阻止创建一个无效的块。因为
        // 允许这样的交易进入交易池将会造成一个类似的DOS攻击。
        //  继续检查该交易，不符合，同进入样不让进入 交易池。
        uint32_t currentBlockScriptVerifyFlags =
            GetBlockScriptFlags(chainActive.Tip(), config);
        if (!CheckInputsFromMempoolAndCache(tx, state, view, pool,
                                            currentBlockScriptVerifyFlags, true,
                                            txdata)) {
            // If we're using promiscuousmempoolflags, we may hit this normally.
            // Check if current block has some flags that scriptVerifyFlags does
            // not before printing an ominous warning. 检查两个标识是否相同
            if (!(~scriptVerifyFlags & currentBlockScriptVerifyFlags)) {
                return error(
                    "%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against "
                    "MANDATORY but not STANDARD flags %s, %s",
                    __func__, txid.ToString(), FormatStateMessage(state));
            }
            //检查交易输入，换了一个检查标识
            if (!CheckInputs(tx, state, view, true,
                             MANDATORY_SCRIPT_VERIFY_FLAGS, true, false,
                             txdata)) {
                return error(
                    "%s: ConnectInputs failed against MANDATORY but not "
                    "STANDARD flags due to promiscuous mempool %s, %s",
                    __func__, txid.ToString(), FormatStateMessage(state));
            }

            LogPrintf("Warning: -promiscuousmempool flags set to not include "
                      "currently enforced soft forks, this may break mining or "
                      "otherwise cause instability!\n");
        }

        // This transaction should only count for fee estimation if
        // the node is not behind and it is not dependent on any other
        // transactions in the mempool.
        //只有当节点不在后面，该交易才可以用来估算费用，并且它不依赖于交易池中的其他交易。
        // ture : 标识当前链可以用来预估交易，且这个交易不依赖于交易池中的其他交易。
        bool validForFeeEstimation =
            IsCurrentForFeeEstimation() && pool.HasNoInputsOf(tx);

        // Store transaction in memory.；在交易池中添加该交易
        pool.addUnchecked(txid, entry, setAncestors, validForFeeEstimation);

        // Trim mempool and check if tx was trimmed.
        // 修剪交易池；并检查该交易是否需要修剪
        if (!fOverrideMempoolLimit) {
            LimitMempoolSize(
                pool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
                GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            //如果交易池中不存在该交易，标识刚才裁剪交易池时，把该交易又减掉了。
            if (!pool.exists(txid)) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                                 "mempool full");
            }
        }
    }
    //发送信号，标识交易池接收到了一个交易
    GetMainSignals().SyncTransaction(
        tx, nullptr, CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);

    return true;
}

static bool AcceptToMemoryPoolWithTime(
    const Config &config, CTxMemPool &pool, CValidationState &state,
    const CTransactionRef &tx, bool fLimitFree, bool *pfMissingInputs,
    int64_t nAcceptTime, std::list<CTransactionRef> *plTxnReplaced = nullptr,
    bool fOverrideMempoolLimit = false, const Amount nAbsurdFee = 0) {


    std::vector<COutPoint> coins_to_uncache;
    // 将这个交易添加进交易池
    bool res = AcceptToMemoryPoolWorker(
        config, pool, state, tx, fLimitFree, pfMissingInputs, nAcceptTime,
        plTxnReplaced, fOverrideMempoolLimit, nAbsurdFee, coins_to_uncache);
    if (!res) {
        // 如果添加失败，可能该交易已在交易池中；将该交易中的花费的UTXO，从UTXO集合的缓存中删除；
        for (const COutPoint &outpoint : coins_to_uncache) {
            pcoinsTip->Uncache(outpoint);
        }
    }

    // After we've (potentially) uncached entries, ensure our coins cache is
    // still within its size limits
    // 刷新数据到磁盘
    CValidationState stateDummy;
    FlushStateToDisk(stateDummy, FLUSH_STATE_PERIODIC);
    return res;
}

// 将一个交易添加进交易池，可能添加失败
bool AcceptToMemoryPool(const Config &config, CTxMemPool &pool,
                        CValidationState &state, const CTransactionRef &tx,
                        bool fLimitFree, bool *pfMissingInputs,
                        std::list<CTransactionRef> *plTxnReplaced,
                        bool fOverrideMempoolLimit, const Amount nAbsurdFee) {
    return AcceptToMemoryPoolWithTime(config, pool, state, tx, fLimitFree,
                                      pfMissingInputs, GetTime(), plTxnReplaced,
                                      fOverrideMempoolLimit, nAbsurdFee);
}

/** Return transaction in txOut, and if it was found inside a block, its hash is
 * placed in hashBlock
 * 如果交易在指定的块中，返回这个交易
 * txout(out):放入查询到的交易；
 * */
bool GetTransaction(const Config &config, const uint256 &txid,
                    CTransactionRef &txOut, uint256 &hashBlock,
                    bool fAllowSlow) {
    CBlockIndex *pindexSlow = nullptr;

    LOCK(cs_main);

    CTransactionRef ptx = mempool.get(txid);
    if (ptx) {
        txOut = ptx;
        return true;
    }

    if (fTxIndex) {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(txid, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK,
                           CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            } catch (const std::exception &e) {
                return error("%s: Deserialize or I/O error - %s", __func__,
                             e.what());
            }
            hashBlock = header.GetHash();
            if (txOut->GetId() != txid)
                return error("%s: txid mismatch", __func__);
            return true;
        }
    }

    // use coin database to locate block that contains transaction, and scan it
    //使用UTXO数据库区定位包含该交易的block， 然后扫描该block；
    if (fAllowSlow) {
        const Coin &coin = AccessByTxid(*pcoinsTip, txid);
        if (!coin.IsSpent()) {
            pindexSlow = chainActive[coin.GetHeight()];
        }
    }

    //
    if (pindexSlow) {
        auto &params = config.GetChainParams().GetConsensus();

        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, params)) {
            for (const auto &tx : block.vtx) {
                if (tx->GetId() == txid) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex

//将块和报头标识符 写入文件
//block(in):将要写入的区块； pos(in/out):赋值块写入信息的起始位置；messagestart(in):写入的报头的信息
bool WriteBlockToDisk(const CBlock &block, CDiskBlockPos &pos,
                      const CMessageHeader::MessageStartChars &messageStart) {
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header       计算块的大小
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << FLATDATA(messageStart) << nSize;     //将报头信息 和 字节大小写入文件

    // Write block      写区块
    long fileOutPos = ftell(fileout.Get());     //获取文件此时的偏移位置
    if (fileOutPos < 0) return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

// 根据block在文件指定文件中的数据偏移位置，读取块数据，并检测读取块的工作量。
bool ReadBlockFromDisk(CBlock &block, const CDiskBlockPos &pos,
                       const Consensus::Params &consensusParams) {
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s",
                     pos.ToString());

    // Read block
    try {
        filein >> block;
    } catch (const std::exception &e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__,
                     e.what(), pos.ToString());
    }

    // Check the header
    if (!CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
        return error("ReadBlockFromDisk: Errors in block header at %s",
                     pos.ToString());

    return true;
}

//从磁盘读取block数据  block(out),读取的数据放入该block结构; pindex(in) 该block 的索引
bool ReadBlockFromDisk(CBlock &block, const CBlockIndex *pindex,
                       const Consensus::Params &consensusParams) {
    //1. 从磁盘的某个位置读取 block数据
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), consensusParams))
        return false;
    //2. 比较读取的block数据 与传入的它的index 的块hash是否相同
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() "
                     "doesn't match index for %s at %s",
                     pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

Amount GetBlockSubsidy(int nHeight, const Consensus::Params &consensusParams) {
    int halvings = nHeight / consensusParams.nSubsidyHalvingInterval;
    // Force block reward to zero when right shift is undefined.
    if (halvings >= 64) return 0;

    Amount nSubsidy = 50 * COIN;
    // Subsidy is cut in half every 210,000 blocks which will occur
    // approximately every 4 years.
    return Amount(nSubsidy.GetSatoshis() >> halvings);
}

//是初始化下载块数据； 是返回TRUE。
bool IsInitialBlockDownload() {
    const CChainParams &chainParams = Params();

    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.在获取锁之前，先获取原子变量
    if (latchToFalse.load(std::memory_order_relaxed)) return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed)) return false;
    if (fImporting || fReindex) return true;
    if (chainActive.Tip() == nullptr) return true;
    if (chainActive.Tip()->nChainWork <
        UintToArith256(chainParams.GetConsensus().nMinimumChainWork))
        return true;
    // 标识当前激活链的Tip块时间 小于 要求出块的时间间隔(这个时间间隔内，肯定出块)，
    // 标识还有块未被接收，此时一定是正在下载块
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - nMaxTipAge))
        return true;
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

CBlockIndex *pindexBestForkTip = nullptr, *pindexBestForkBase = nullptr;

static void AlertNotify(const std::string &strMessage) {
    uiInterface.NotifyAlertChanged();
    std::string strCmd = GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to be
    // safe we first strip anything not in safeChars, then add single quotes
    // around the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote + safeStatus + singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    boost::thread t(runCommand, strCmd); // thread runs free
}

void CheckForkWarningConditions() {
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial
    // sync)
    if (IsInitialBlockDownload()) return;

    // If our best fork is no longer within 72 blocks (+/- 12 hours if no one
    // mines it) of our head, drop it
    if (pindexBestForkTip &&
        chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = nullptr;

    if (pindexBestForkTip ||
        (pindexBestInvalid &&
         pindexBestInvalid->nChainWork >
             chainActive.Tip()->nChainWork +
                 (GetBlockProof(*chainActive.Tip()) * 6))) {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase) {
            std::string warning =
                std::string("'Warning: Large-work fork detected, forking after "
                            "block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            AlertNotify(warning);
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            LogPrintf("%s: Warning: Large valid fork found\n  forking the "
                      "chain at height %d (%s)\n  lasting to height %d "
                      "(%s).\nChain state database corruption likely.\n",
                      __func__, pindexBestForkBase->nHeight,
                      pindexBestForkBase->phashBlock->ToString(),
                      pindexBestForkTip->nHeight,
                      pindexBestForkTip->phashBlock->ToString());
            SetfLargeWorkForkFound(true);
        } else {
            LogPrintf("%s: Warning: Found invalid chain at least ~6 blocks "
                      "longer than our best chain.\nChain state database "
                      "corruption likely.\n",
                      __func__);
            SetfLargeWorkInvalidChainFound(true);
        }
    } else {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex *pindexNewForkTip) {
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex *pfork = pindexNewForkTip;
    CBlockIndex *plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger) break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at
    // least 7 blocks with a tip within 72 blocks (+/- 12 hours if no one mines
    // it) of ours. We use 7 blocks rather arbitrarily as it represents just
    // under 10% of sustained network hash rate operating on the fork, or a
    // chain that is entirely longer than ours and invalid (note that this
    // should be detected by both). We define it this way because it allows us
    // to only store the highest fork tip (+ base) which meets the 7-block
    // condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip ||
                  (pindexBestForkTip &&
                   pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork >
            (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

static void InvalidChainFound(CBlockIndex *pindexNew) {
    if (!pindexBestInvalid ||
        pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf(
        "%s: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexNew->GetBlockTime()));
    CBlockIndex *tip = chainActive.Tip();
    assert(tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
              __func__, tip->GetBlockHash().ToString(), chainActive.Height(),
              log(tip->nChainWork.getdouble()) / log(2.0),
              DateTimeStrFormat("%Y-%m-%d %H:%M:%S", tip->GetBlockTime()));
    CheckForkWarningConditions();
}

static void InvalidBlockFound(CBlockIndex *pindex,
                              const CValidationState &state) {
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_\
 ]FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

//tx(in):更新到UTXO中的交易； input(in):UTXO集合； txundo(in/out):获取该交易的undo信息(即当前交易花费的UTXO集合)，然后写入文件。
//nHeight(in):该交易的高度；
void UpdateCoins(const CTransaction &tx, CCoinsViewCache &inputs,
                 CTxUndo &txundo, int nHeight) {
    // Mark inputs spent. 标识交易输入花费
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn &txin : tx.vin) {
            txundo.vprevout.emplace_back();
            bool is_spent =
                inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
            assert(is_spent);
        }
    }

    // Add outputs. 添加交易输出
    AddCoins(inputs, tx, nHeight);
}

void UpdateCoins(const CTransaction &tx, CCoinsViewCache &inputs, int nHeight) {
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()() {
    //获取将要检查交易的第n个交易输入的签名脚本。
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    //验证这个交易输入的签名是否正确。
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags,
                      CachingTransactionSignatureChecker(ptxTo, nIn, amount,
                                                         cacheStore, txdata),
                      &error)) {
        return false;
    }
    return true;
}

int GetSpendHeight(const CCoinsViewCache &inputs) {
    LOCK(cs_main);
    CBlockIndex *pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus {
//
bool CheckTxInputs(const CTransaction &tx, CValidationState &state,
                   const CCoinsViewCache &inputs, int nSpendHeight) {
    // This doesn't trigger the DoS code on purpose; if it did, it would make it
    // easier for an attacker to attempt to split the network.
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(false, 0, "", "Inputs unavailable");
    }

    Amount nValueIn = 0;
    Amount nFees = 0;
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin &coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase()) {
            if (nSpendHeight - coin.GetHeight() < COINBASE_MATURITY) {
                return state.Invalid(
                    false, REJECT_INVALID,
                    "bad-txns-premature-spend-of-coinbase",
                    strprintf("tried to spend coinbase at depth %d",
                              nSpendHeight - coin.GetHeight()));
            }
        }

        // Check for negative or overflow input values
        nValueIn += coin.GetTxOut().nValue.GetSatoshis();
        if (!MoneyRange(coin.GetTxOut().nValue) || !MoneyRange(nValueIn)) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-inputvalues-outofrange");
        }
    }

    if (nValueIn < tx.GetValueOut()) {
        return state.DoS(
            100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
            strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn),
                      FormatMoney(tx.GetValueOut().GetSatoshis())));
    }

    // Tally transaction fees
    Amount nTxFee = nValueIn - tx.GetValueOut();
    if (nTxFee < 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
    }
    nFees += nTxFee;
    if (!MoneyRange(nFees)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    }

    return true;
}
} // namespace Consensus

//检查一个所有的交易输入
//tx(in):检查的交易；state(out):检查后的状态；inputs(in):存储该交易的引用输出的UTXO数据，
//检查脚本(默认TRUE)；验证标识(默认为标准检查)；签名缓存；脚本缓存；txdata(in):计算参一的各类哈希
bool CheckInputs(const CTransaction &tx, CValidationState &state,
                 const CCoinsViewCache &inputs, bool fScriptChecks,
                 uint32_t flags, bool sigCacheStore, bool scriptCacheStore,
                 const PrecomputedTransactionData &txdata,
                 std::vector<CScriptCheck> *pvChecks) {
    assert(!tx.IsCoinBase());
    //检查交易输入
    if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs))) {
        return false;
    }
    // 存储该交易的所有验证条件。
    if (pvChecks) {
        pvChecks->reserve(tx.vin.size());
    }

    // The first loop above does all the inexpensive checks. Only if ALL inputs
    // pass do we perform expensive ECDSA signature checks. Helps prevent CPU
    // exhaustion attacks.

    // Skip script verification when connecting blocks under the assumedvalid
    // block. Assuming the assumedvalid block is valid this is safe because
    // block merkle hashes are still computed and checked, of course, if an
    // assumed valid block is invalid due to false scriptSigs this optimization
    // would allow an invalid chain to be accepted.
    if (!fScriptChecks) {
        return true;
    }

    // First check if script executions have been cached with the same flags.
    // Note that this assumes that the inputs provided are correct (ie that the
    // transaction hash which is in tx's prevouts properly commits to the
    // scriptPubKey in the inputs view of that transaction).
    uint256 hashCacheEntry = GetScriptCacheKey(tx, flags);
    if (IsKeyInScriptCache(hashCacheEntry, !scriptCacheStore)) {
        return true;
    }

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const COutPoint &prevout = tx.vin[i].prevout;
        // 获取交易的某个交易输入的 引用输出的锁定脚本和金额
        const Coin &coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // We very carefully only pass in things to CScriptCheck which are
        // clearly committed to by tx' witness hash. This provides a sanity
        // check that our caching is not introducing consensus failures through
        // additional data in, eg, the coins being spent being checked as a part
        // of CScriptCheck.
        // 我们非常谨慎的只将验证信息传递给 CScriptCheck 对象，然后将该对象传递到验证队列中，让任务线程进行处理。
        const CScript &scriptPubKey = coin.GetTxOut().scriptPubKey;
        const Amount amount = coin.GetTxOut().nValue;

        // Verify signature； 构建脚本验证；放入检查队列中，然后添加进全局的队列。
        CScriptCheck check(scriptPubKey, amount, tx, i, flags, sigCacheStore,
                           txdata);
        if (pvChecks) {
            pvChecks->push_back(std::move(check));
        } else if (!check()) {      //标准flag检查失败后，进入此处；
            // 下面主要是用来判断非强制性脚本检查的状态，不管是否成功都退出。
            //即当没有全局任务队列时，只有主线程验证所有的交易；
            if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                // Check whether the failure was caused by a non-mandatory
                // script verification check, such as non-standard DER encodings
                // or non-null dummy arguments; if so, don't trigger DoS
                // protection to avoid splitting the network between upgraded
                // and non-upgraded nodes.
                CScriptCheck check2(scriptPubKey, amount, tx, i,
                                    flags &
                                        ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS,
                                    sigCacheStore, txdata);
                if (check2()) {
                    return state.Invalid(
                        false, REJECT_NONSTANDARD,
                        strprintf("non-mandatory-script-verify-flag (%s)",
                                  ScriptErrorString(check.GetScriptError())));
                }
            }

            // Failures of other flags indicate a transaction that is invalid in
            // new blocks, e.g. a invalid P2SH. We DoS ban such nodes as they
            // are not following the protocol. That said during an upgrade
            // careful thought should be taken as to the correct behavior - we
            // may want to continue peering with non-upgraded nodes even after
            // soft-fork super-majority signaling has occurred.
            return state.DoS(
                100, false, REJECT_INVALID,
                strprintf("mandatory-script-verify-flag-failed (%s)",
                          ScriptErrorString(check.GetScriptError())));
        }
    }

    // 将该交易哈希放入缓存中
    if (scriptCacheStore && !pvChecks) {
        // We executed all of the provided scripts, and were told to cache the
        // result. Do so now.  我们执行所有提供的脚本，并把这些脚本对应的交易添加进缓存。
        // 以便指定的交易只被验证一次签名。
        AddKeyInScriptCache(hashCacheEntry);
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo &blockundo, CDiskBlockPos &pos,
                     const uint256 &hashBlock,
                     const CMessageHeader::MessageStartChars &messageStart) {
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, blockundo);
    fileout << FLATDATA(messageStart) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

//  blockundo(out) 读取undo数据放在该结构中； pos:undo数据在文件中的位置； hashblock:上一个块的哈希
bool UndoReadFromDisk(CBlockUndo &blockundo, const CDiskBlockPos &pos,
                      const uint256 &hashBlock) {
    // Open history file to read 打开文件
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Read block
    uint256 hashChecksum;
    // We need a CHashVerifier as reserializing may lose data；  采用文件描述符作为底层数据层，创建一个对象
    CHashVerifier<CAutoFile> verifier(&filein);
    try {
        verifier << hashBlock;      //将hashblock 写入文件
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception &e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash()) {
        return error("%s: Checksum mismatch", __func__);
    }

    return true;
}

/** Abort with a message */
bool AbortNode(const std::string &strMessage,
               const std::string &userMessage = "") {
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see "
                                "debug.log for details")
                            : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState &state, const std::string &strMessage,
               const std::string &userMessage = "") {
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

} // namespace

/** Restore the UTXO in a Coin at a given COutPoint.
 * 依据给定交易输出重新存储UTXO
 * Out(in):引用交易输出；undo(in):该引用输出对应的锁定脚本；view(in):UTXO视角
 * */
DisconnectResult UndoCoinSpend(const Coin &undo, CCoinsViewCache &view,
                               const COutPoint &out) {
    bool fClean = true;
    //1. UTXO集合中存在该引用输出。
    if (view.HaveCoin(out)) {
        // Overwriting transaction output.
        fClean = false;
    }

    //2. 丢失了undo的原始数据(高度和是否为coinbase)；
    if (undo.GetHeight() == 0) {
        // Missing undo metadata (height and coinbase). Older versions included
        // this information only in undo records for the last spend of a
        // transactions' outputs. This implies that it must be present for some
        // other output of the same tx.
        // 对于最近花费的一个交易输出，旧版本中的这些信息指挥记录在undo结构中。这意味着它必须存在于相同交易的其他输出中。
        // 通过交易的哈希访问UTXO集合，获取
        const Coin &alternate = AccessByTxid(view, out.hash);
        //如果返回的coin已被花费
        if (alternate.IsSpent()) {
            // Adding output for transaction without known metadata
            return DISCONNECT_FAILED;
        }

        // This is somewhat ugly, but hopefully utility is limited. This is only
        // useful when working from legacy on disck data. In any case, putting
        // the correct information in there doesn't hurt.
        // 这种方法有点丑陋，但希望效用是有限的。只有当使用以前的磁盘数据格式时才是有效的。在所有的示例中，
        // 放正确的信息在这路不会造成伤害。
        const_cast<Coin &>(undo) = Coin(undo.GetTxOut(), alternate.GetHeight(),
                                        alternate.IsCoinBase());
    }

    //向UTXO集合中添加coin。
    view.AddCoin(out, undo, undo.IsCoinBase());
    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

/**
 * Undo the effects of this block (with given index) on the UTXO set represented
 * by coins. When UNCLEAN or FAILED is returned, view is left in an
 * indeterminate state.
 * 撤销这个块在UTXO集合上的影响。当UNCLEAN或FAILED状态返回时，UTXO集合视角将处于不定的状态
 * 参二 是 参一的块索引。
 */
static DisconnectResult DisconnectBlock(const CBlock &block,
                                        const CBlockIndex *pindex,
                                        CCoinsViewCache &view) {
    //1. 拒绝的区块必须是UTXO集合中的最高块
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    CBlockUndo blockUndo;

    // 获取pindex 中undo数据所在的文件位置
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        error("DisconnectBlock(): no undo data available");
        return DISCONNECT_FAILED;
    }

    // 从磁盘读取undo数据
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash())) {
        error("DisconnectBlock(): failure reading undo data");
        return DISCONNECT_FAILED;
    }

    // 应用块的撤销文件，撤销块
    return ApplyBlockUndo(blockUndo, block, pindex, view);
}

//blockUndo(in):该块的撤销区块；CBlock(in):该块的数据；pindex(in):该块的索引；view(in):当前全局状态的UTXO集合的视角
//撤销一个区块
DisconnectResult ApplyBlockUndo(const CBlockUndo &blockUndo,
                                const CBlock &block, const CBlockIndex *pindex,
                                CCoinsViewCache &view) {
    bool fClean = true;
    //1. 先检查块的 undo的交易和块的交易数量
    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        error("DisconnectBlock(): block and undo data inconsistent");
        return DISCONNECT_FAILED;
    }

    // Undo transactions in reverse order.
    // 反向撤销 block中的 交易数据
    size_t i = block.vtx.size();
    while (i-- > 0) {

        const CTransaction &tx = *(block.vtx[i]);
        uint256 txid = tx.GetId();

        // Check that all outputs are available and match the outputs in the
        // block itself exactly. 遍历所有的交易输出，清除在UTXO集合中存在的输出。
        for (size_t o = 0; o < tx.vout.size(); o++) {
            //不可花费的交易不用撤销(因为它没有记录在UTXO集合中)；继续下次循环；
            if (tx.vout[o].scriptPubKey.IsUnspendable()) {
                continue;
            }

            //构建一个引用交易输出
            COutPoint out(txid, o);
            Coin coin;
            //标识该UTXO已被花掉，即相当于删除
            bool is_spent = view.SpendCoin(out, &coin);
            //如果没有在UTXO集合中找到这个引用输出；或找到的引用输出不匹配
            if (!is_spent || tx.vout[o] != coin.GetTxOut()) {
                // transaction output mismatch  清除失败
                fClean = false;
            }
        }

        // Restore inputs.  跳过coinbase交易
        if (i < 1) {
            // Skip the coinbase.
            continue;
        }
        //比较undo交易与长正常交易的数量
        const CTxUndo &txundo = blockUndo.vtxundo[i - 1];
        if (txundo.vprevout.size() != tx.vin.size()) {
            error("DisconnectBlock(): transaction and undo data inconsistent");
            return DISCONNECT_FAILED;
        }

        //
        for (size_t j = tx.vin.size(); j-- > 0;) {
            // 获取交易的引用输出以及锁定脚本
            const COutPoint &out = tx.vin[j].prevout;
            const Coin &undo = txundo.vprevout[j];
            DisconnectResult res = UndoCoinSpend(undo, view, out);
            if (res == DISCONNECT_FAILED) {
                return DISCONNECT_FAILED;
            }
            fClean = fClean && res != DISCONNECT_UNCLEAN;
        }
    }

    // Move best block pointer to previous block.
    view.SetBestBlock(block.hashPrevBlock);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

//刷新磁盘文件；fFinalize:true，标识不知道当前文件，打开全局记录的文件描述符；false:知道当前文件。
static void FlushBlockFile(bool fFinalize = false) {
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    //打开文件，并将文件描述符偏移一定的位置； 截断一个文件，然后同步文件的状态
    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)      //不知道要写的文件，此时去更新全局的缓存文件
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);        //截断文件
        FileCommit(fileOld);        //同步文件的元数据
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)      //不知道要写的文件，此时去更新全局的缓存文件
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos,
                 unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);     //脚本检查队列的对象。存储了调用这个对象的线程总线程数量，和

//脚本检查
void ThreadScriptCheck() {
    RenameThread("bitcoin-scriptch");
    scriptcheckqueue.Thread();      //一个对象，被多个线程调用。该对象内部存储了脚本验证的任务，和当前总的任务数。
}

// Protected by cs_main
VersionBitsCache versionbitscache;

//计算块的版本；
int32_t ComputeBlockVersion(const CBlockIndex *pindexPrev,
                            const Consensus::Params &params) {
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        ThresholdState state = VersionBitsState(
            pindexPrev, params, (Consensus::DeploymentPos)i, versionbitscache);
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED) {
            nVersion |= VersionBitsMask(params, (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}

/**
 * Threshold condition checker that triggers when unknown versionbits are seen
 * on the network.
 */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker {
private:
    int bit;

public:
    WarningBitsConditionChecker(int bitIn) : bit(bitIn) {}

    int64_t BeginTime(const Consensus::Params &params) const { return 0; }
    int64_t EndTime(const Consensus::Params &params) const {
        return std::numeric_limits<int64_t>::max();
    }
    int Period(const Consensus::Params &params) const {
        return params.nMinerConfirmationWindow;
    }
    int Threshold(const Consensus::Params &params) const {
        return params.nRuleChangeActivationThreshold;
    }

    bool Condition(const CBlockIndex *pindex,
                   const Consensus::Params &params) const {
        return ((pindex->nVersion & VERSIONBITS_TOP_MASK) ==
                VERSIONBITS_TOP_BITS) &&
               ((pindex->nVersion >> bit) & 1) != 0 &&
               ((ComputeBlockVersion(pindex->pprev, params) >> bit) & 1) == 0;
    }
};

// Protected by cs_main
static ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

// Returns the script flags which should be checked for a given block；
// 返回检查的块的脚本标识。
static uint32_t GetBlockScriptFlags(const CBlockIndex *pindex,
                                    const Config &config) {
    AssertLockHeld(cs_main);
    const Consensus::Params &consensusparams =
        config.GetChainParams().GetConsensus();

    // BIP16 didn't become active until Apr 1 2012
    int64_t nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pindex->GetBlockTime() >= nBIP16SwitchTime);

    uint32_t flags =
        fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;

    // Start enforcing the DERSIG (BIP66) rule
    if (pindex->nHeight >= consensusparams.BIP66Height) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Start enforcing CHECKLOCKTIMEVERIFY (BIP65) rule
    if (pindex->nHeight >= consensusparams.BIP65Height) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Start enforcing BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    if (VersionBitsState(pindex->pprev, consensusparams,
                         Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    }

    // If the UAHF is enabled, we start accepting replay protected txns
    if (IsUAHFenabled(config, pindex->pprev)) {
        flags |= SCRIPT_VERIFY_STRICTENC;
        flags |= SCRIPT_ENABLE_SIGHASH_FORKID;
    }

    // If the Cash HF is enabled, we start rejecting transaction that use a high
    // s in their signature. We also make sure that signature that are supposed
    // to fail (for instance in multisig or other forms of smart contracts) are
    // null.
    if (IsCashHFEnabled(config, pindex->pprev)) {
        flags |= SCRIPT_VERIFY_LOW_S;
        flags |= SCRIPT_VERIFY_NULLFAIL;
    }

    return flags;
}

static int64_t nTimeCheck = 0;
static int64_t nTimeForks = 0;
static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

/**
 * Apply the effects of this block (with given index) on the UTXO set
 * represented by coins. Validity checks that depend on the UTXO set are also
 * done; ConnectBlock() can fail if those validity checks fail (among other
 * reasons).
 * block(in):将要链接到激活链上的区块(带有完整数据)； pindex(in):该链接块对应的索引；
 */
static bool ConnectBlock(const Config &config, const CBlock &block,
                         CValidationState &state, CBlockIndex *pindex,
                         CCoinsViewCache &view, const CChainParams &chainparams,
                         bool fJustCheck = false) {
    AssertLockHeld(cs_main);

    int64_t nTimeStart = GetTimeMicros();

    // Check it again in case a previous version let a bad block in
    // 再次检查将要链接的区块；(防止坏块链接到激活链上)
    if (!CheckBlock(config, block, state, chainparams.GetConsensus(),
                    !fJustCheck, !fJustCheck)) {
        return error("%s: Consensus::CheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }

    // Verify that the view's current state corresponds to the previous block
    // 获取该连接块的父区块
    uint256 hashPrevBlock =
        pindex->pprev == nullptr ? uint256() : pindex->pprev->GetBlockHash();
    // 它的父区块必须是当前UTXO集合的Tip块。
    assert(hashPrevBlock == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its
    // transactions (its coinbase is unspendable)
    // 如果当前链接的块为创世块，直接设置它的索引为UTXO集合的Tip块。
    if (block.GetHash() == chainparams.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck) {
            view.SetBestBlock(pindex->GetBlockHash());
        }

        return true;
    }

    bool fScriptChecks = true;
    // 该区块含有实际的数据，进入下列条件
    if (!hashAssumeValid.IsNull()) {
        // We've been configured with the hash of a block which has been
        // externally verified to have a valid history. A suitable default value
        // is included with the software and updated from time to time. Because
        // validity relative to a piece of software is an objective fact these
        // defaults can be easily reviewed. This setting doesn't force the
        // selection of any particular chain but makes validating some faster by
        // effectively caching the result of part of the verification.
        // 在全局状态中查找这个区块
        BlockMap::const_iterator it = mapBlockIndex.find(hashAssumeValid);
        if (it != mapBlockIndex.end()) {
            //
            if (it->second->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->nChainWork >=
                    UintToArith256(
                        chainparams.GetConsensus().nMinimumChainWork)) {
                // This block is a member of the assumed verified chain and an
                // ancestor of the best header. The equivalent time check
                // discourages hashpower from extorting the network via DOS
                // attack into accepting an invalid block through telling users
                // they must manually set assumevalid. Requiring a software
                // change or burying the invalid block, regardless of the
                // setting, makes it hard to hide the implication of the demand.
                // This also avoids having release candidates that are hardly
                // doing any signature verification at all in testing without
                // having to artificially set the default assumed verified block
                // further back. The test against nMinimumChainWork prevents the
                // skipping when denied access to any chain at least as good as
                // the expected chain.
                fScriptChecks =
                    (GetBlockProofEquivalentTime(
                         *pindexBestHeader, *pindex, *pindexBestHeader,
                         chainparams.GetConsensus()) <= 60 * 60 * 24 * 7 * 2);
            }
        }
    }

    int64_t nTime1 = GetTimeMicros();
    nTimeCheck += nTime1 - nTimeStart;
    LogPrint("bench", "    - Sanity checks: %.2fms [%.2fs]\n",
             0.001 * (nTime1 - nTimeStart), nTimeCheck * 0.000001);

    // Do not allow blocks that contain transactions which 'overwrite' older
    // transactions, unless those are already completely spent. If such
    // overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance --
    // even after being sent to another address. See BIP30 and
    // http://r6.ca/blog/20120206T005236Z.html for more information. This logic
    // is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely. This rule was
    // originally applied to all blocks with a timestamp after March 15, 2012,
    // 0:00 UTC. Now that the whole chain is irreversibly beyond that time it is
    // applied to all blocks except the two in the chain that violate it. This
    // prevents exploiting the issue against nodes during their initial block
    // download.
    bool fEnforceBIP30 = (!pindex->phashBlock) || // Enforce on CreateNewBlock
                                                  // invocations which don't
                                                  // have a hash.
                         !((pindex->nHeight == 91842 &&
                            pindex->GetBlockHash() ==
                                uint256S("0x00000000000a4d0a398161ffc163c503763"
                                         "b1f4360639393e0e4c8e300e0caec")) ||
                           (pindex->nHeight == 91880 &&
                            pindex->GetBlockHash() ==
                                uint256S("0x00000000000743f190a18c5577a3c2d2a1f"
                                         "610ae9601ac046a38084ccb7cd721")));

    // Once BIP34 activated it was not possible to create new duplicate
    // coinbases and thus other than starting with the 2 existing duplicate
    // coinbase pairs, not possible to create overwriting txs. But by the time
    // BIP34 activated, in each of the existing pairs the duplicate coinbase had
    // overwritten the first before the first had been spent. Since those
    // coinbases are sufficiently buried its no longer possible to create
    // further duplicate transactions descending from the known pairs either. If
    // we're on the known chain at height greater than where BIP34 activated, we
    // can save the db accesses needed for the BIP30 check.
    CBlockIndex *pindexBIP34height =
        pindex->pprev->GetAncestor(chainparams.GetConsensus().BIP34Height);
    // Only continue to enforce if we're below BIP34 activation height or the
    // block hash at that height doesn't correspond.
    fEnforceBIP30 = fEnforceBIP30 && (!pindexBIP34height ||
                                      !(pindexBIP34height->GetBlockHash() ==
                                        chainparams.GetConsensus().BIP34Hash));
    // 如果fEnforceBIP30为TRUE，进行共识检查
    if (fEnforceBIP30) {
        for (const auto &tx : block.vtx) {
            for (size_t o = 0; o < tx->vout.size(); o++) {
                if (view.HaveCoin(COutPoint(tx->GetHash(), o))) {
                    return state.DoS(
                        100,
                        error("ConnectBlock(): tried to overwrite transaction"),
                        REJECT_INVALID, "bad-txns-BIP30");
                }
            }
        }
    }

    // Start enforcing BIP68 (sequence locks) using versionbits logic.
    // 强制执行BIP68 逻辑
    int nLockTimeFlags = 0;
    if (VersionBitsState(pindex->pprev, chainparams.GetConsensus(),
                         Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    // 根据块的状态，获取块的验证标识。
    uint32_t flags = GetBlockScriptFlags(pindex, config);

    int64_t nTime2 = GetTimeMicros();
    nTimeForks += nTime2 - nTime1;
    LogPrint("bench", "    - Fork checks: %.2fms [%.2fs]\n",
             0.001 * (nTime2 - nTime1), nTimeForks * 0.000001);

    // 此处构造块的 undo信息； 存储的是 交易的undo信息
    CBlockUndo blockundo;
    CCheckQueueControl<CScriptCheck> control(fScriptChecks ? &scriptcheckqueue
                                                           : nullptr);

    std::vector<int> prevheights;
    Amount nFees = 0;
    int nInputs = 0;

    // Sigops counting. We need to do it again because of P2SH.
    uint64_t nSigOpsCount = 0;
    const uint64_t currentBlockSize =
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    const uint64_t nMaxSigOpsCount = GetMaxBlockSigOpsCount(currentBlockSize);

    CDiskTxPos pos(pindex->GetBlockPos(),
                   GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos>> vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);

    // 检查该区块中含的交易； 将将块中所有交易的所有签名验证添加到全局队列中。
    for (size_t i = 0; i < block.vtx.size(); i++) {
        const CTransaction &tx = *(block.vtx[i]);

        nInputs += tx.vin.size();

        if (!tx.IsCoinBase()) {
            // 检查该区块中含的交易的引用输出是否存在于UTXO；不存在，直接返回出错
            if (!view.HaveInputs(tx)) {
                return state.DoS(
                    100, error("ConnectBlock(): inputs missing/spent"),
                    REJECT_INVALID, "bad-txns-inputs-missingorspent");
            }

            // Check that transaction is BIP68 final BIP68 lock checks (as
            // opposed to nLockTime checks) must be in ConnectBlock because they
            // require the UTXO set.
            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                prevheights[j] = view.AccessCoin(tx.vin[j].prevout).GetHeight();
            }

            if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *pindex)) {
                return state.DoS(
                    100, error("%s: contains a non-BIP68-final transaction",
                               __func__),
                    REJECT_INVALID, "bad-txns-nonfinal");
            }
        }

        // GetTransactionSigOpCount counts 2 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        auto txSigOpsCount = GetTransactionSigOpCount(tx, view, flags);
        if (txSigOpsCount > MAX_TX_SIGOPS_COUNT) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txn-sigops");
        }

        nSigOpsCount += txSigOpsCount;
        if (nSigOpsCount > nMaxSigOpsCount) {
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");
        }

        if (!tx.IsCoinBase()) {
            Amount fee = view.GetValueIn(tx) - tx.GetValueOut();
            nFees += fee.GetSatoshis();

            // Don't cache results if we're actually connecting blocks (still
            // consult the cache, though).
            bool fCacheResults = fJustCheck;

            std::vector<CScriptCheck> vChecks;
            // 检查交易的所有交易输入
            if (!CheckInputs(tx, state, view, fScriptChecks, flags,
                             fCacheResults, fCacheResults,
                             PrecomputedTransactionData(tx), &vChecks)) {
                return error("ConnectBlock(): CheckInputs on %s failed with %s",
                             tx.GetId().ToString(), FormatStateMessage(state));
            }

            control.Add(vChecks);       //向验证线程中添加任务；添加完后，此时其他的任务线程就开始执行。
        }

        CTxUndo undoDummy;      //coinbase 交易下，将该变量传入UpdateCoins中；
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(),
                    pindex->nHeight);

        vPos.push_back(std::make_pair(tx.GetId(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    int64_t nTime3 = GetTimeMicros();
    nTimeConnect += nTime3 - nTime2;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, "
                      "%.3fms/txin) [%.2fs]\n",
             (unsigned)block.vtx.size(), 0.001 * (nTime3 - nTime2),
             0.001 * (nTime3 - nTime2) / block.vtx.size(),
             nInputs <= 1 ? 0 : 0.001 * (nTime3 - nTime2) / (nInputs - 1),
             nTimeConnect * 0.000001);

    Amount blockReward =
        nFees + GetBlockSubsidy(pindex->nHeight, chainparams.GetConsensus());
    if (block.vtx[0]->GetValueOut() > blockReward) {
        return state.DoS(100, error("ConnectBlock(): coinbase pays too much "
                                    "(actual=%d vs limit=%d)",
                                    block.vtx[0]->GetValueOut(), blockReward),
                         REJECT_INVALID, "bad-cb-amount");
    }

    // 执行全局队列中所有的任务； 多线程运行。此时主线程，多线程同时进行检查。
    // 此时该 Wait() 函数退出有两种情况：1. 所有的任务全部成功执行完毕,主线程退出，子线程阻塞；
    // 2.部分任务检查过程中出错，
    if (!control.Wait()) {
        return state.DoS(100, false, REJECT_INVALID, "blk-bad-inputs", false,
                         "parallel script check failed");
    }

    int64_t nTime4 = GetTimeMicros();
    nTimeVerify += nTime4 - nTime2;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n",
             nInputs - 1, 0.001 * (nTime4 - nTime2),
             nInputs <= 1 ? 0 : 0.001 * (nTime4 - nTime2) / (nInputs - 1),
             nTimeVerify * 0.000001);

    if (fJustCheck) {
        return true;
    }

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() ||
        !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos _pos;
            if (!FindUndoPos(
                    state, pindex->nFile, _pos,
                    ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) +
                        40)) {
                return error("ConnectBlock(): FindUndoPos failed");
            }
            if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(),
                                 chainparams.MessageStart())) {
                return AbortNode(state, "Failed to write undo data");
            }

            // update nUndoPos in block index
            pindex->nUndoPos = _pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex && !pblocktree->WriteTxIndex(vPos)) {
        return AbortNode(state, "Failed to write transaction index");
    }

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime5 = GetTimeMicros();
    nTimeIndex += nTime5 - nTime4;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n",
             0.001 * (nTime5 - nTime4), nTimeIndex * 0.000001);

    // Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    GetMainSignals().UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0]->GetId();

    int64_t nTime6 = GetTimeMicros();
    nTimeCallbacks += nTime6 - nTime5;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n",
             0.001 * (nTime6 - nTime5), nTimeCallbacks * 0.000001);

    return true;
}

/**
 * Update the on-disk chain state. 更新磁盘上的链状态
 * The caches and indexes are flushed depending on the mode we're called with if
 * they're too large, if it's been a while since the last write, or always and
 * in all cases if we're in prune mode and are deleting files.
 *
 * 如果缓存和index太大，则依据调用时传递的状态码，将它们刷新到磁盘。如果从最后一次开始写入已经过了
 * 一段时间，或一直处于修剪模式或者正在删除文件的状态下。
 */
static bool FlushStateToDisk(CValidationState &state, FlushStateMode mode,
                             int nManualPruneHeight) {
    // 获取内存池的使用量，和当前的链参数
    int64_t nMempoolUsage = mempool.DynamicMemoryUsage();
    const CChainParams &chainparams = Params();
    LOCK2(cs_main, cs_LastBlockFile);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    std::set<int> setFilesToPrune;

    bool fFlushForPrune = false;
    try {
        // 如果处于修剪模式下，并且修剪的高度>0, 且不处于reindex模式。
        if (fPruneMode && (fCheckForPruning || nManualPruneHeight > 0) &&
            !fReindex) {
            if (nManualPruneHeight > 0) {
                FindFilesToPruneManual(setFilesToPrune, nManualPruneHeight);
            } else {
                FindFilesToPrune(setFilesToPrune,
                                 chainparams.PruneAfterHeight());
                fCheckForPruning = false;
            }
            // 找到裁剪的数据;
            if (!setFilesToPrune.empty()) {
                fFlushForPrune = true;
                // 如果全局变量为不裁剪，修改全局变量
                if (!fHavePruned) {
                    pblocktree->WriteFlag("prunedblockfiles", true);
                    fHavePruned = true;
                }
            }
        }
        // 获取时间
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.  避免启动之后立即写盘。
        if (nLastWrite == 0) {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0) {
            nLastFlush = nNow;
        }
        if (nLastSetChain == 0) {
            nLastSetChain = nNow;
        }
        // 获取 数据限制。
        int64_t nMempoolSizeMax =
            GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
        int64_t cacheSize =
            pcoinsTip->DynamicMemoryUsage() * DB_PEAK_USAGE_FACTOR;
        int64_t nTotalSpace =
            nCoinCacheUsage +
            std::max<int64_t>(nMempoolSizeMax - nMempoolUsage, 0);

        // The cache is large and we're within 10% and 200 MiB or 50% and 50MiB
        // of the limit, but we have time now (not in the middle of a block
        // processing).
        bool fCacheLarge =
            mode == FLUSH_STATE_PERIODIC &&
            cacheSize >
                std::min(std::max(nTotalSpace / 2,
                                  nTotalSpace -
                                      MIN_BLOCK_COINSDB_USAGE * 1024 * 1024),
                         std::max((9 * nTotalSpace) / 10,
                                  nTotalSpace -
                                      MAX_BLOCK_COINSDB_USAGE * 1024 * 1024));
        // The cache is over the limit, we have to write now.
        bool fCacheCritical =
            mode == FLUSH_STATE_IF_NEEDED && cacheSize > nTotalSpace;
        // It's been a while since we wrote the block index to disk. Do this
        // frequently, so we don't need to redownload after a crash.
        // 因为已经将块的索引写入磁盘一段时间了。经常这样操作，所以当软件崩溃后，我们不需要重新下载。
        bool fPeriodicWrite =
            mode == FLUSH_STATE_PERIODIC &&
            nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently,
        // to optimize cache usage.
        bool fPeriodicFlush =
            mode == FLUSH_STATE_PERIODIC &&
            nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge ||
                            fCacheCritical || fPeriodicFlush || fFlushForPrune;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite) {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(0)) return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block
            // and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo *>> vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                // 缓存全局状态中脏的文件信息，并删除全局状态中该数据
                for (std::set<int>::iterator it = setDirtyFileInfo.begin();
                     it != setDirtyFileInfo.end();) {
                    vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex *> vBlocks;
                // 缓存全局状态中脏的块索引数据， 并删除全局状态中该数据
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex *>::iterator it =
                         setDirtyBlockIndex.begin();
                     it != setDirtyBlockIndex.end();) {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }
                //将所有的文件信息，块索引信息，最后一个块，都写入数据库中
                if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile,
                                                vBlocks)) {
                    return AbortNode(state,
                                     "Failed to write to block index database");
                }
            }
            // Finally remove any pruned files 最后移除所有的裁剪文件
            if (fFlushForPrune) UnlinkPrunedFiles(setFilesToPrune);
            nLastWrite = nNow;
        }
        // Flush best chain related state. This can only be done if the blocks /
        // block index write was also done.
        if (fDoFullFlush) {
            // Typical Coin structures on disk are around 48 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(48 * 2 * 2 * pcoinsTip->GetCacheSize())) {
                return state.Error("out of disk space");
            }
            // Flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush()) {
                return AbortNode(state, "Failed to write to coin database");
            }
            nLastFlush = nNow;
        }
        // 向钱包进行更新当前激活链的信息
        if (fDoFullFlush ||
            ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) &&
             nNow >
                 nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000)) {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(chainActive.GetLocator());
            nLastSetChain = nNow;
        }
    } catch (const std::runtime_error &e) {
        return AbortNode(state, std::string("System error while flushing: ") +
                                    e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

void PruneAndFlush() {
    CValidationState state;
    fCheckForPruning = true;
    FlushStateToDisk(state, FLUSH_STATE_NONE);
}

/** Update chainActive and related internal data structures. */
// pindexNew(in):将当前的块索引作为激活的Tip
static void UpdateTip(const Config &config, CBlockIndex *pindexNew) {
    const CChainParams &chainParams = config.GetChainParams();

    chainActive.SetTip(pindexNew);

    // New best block
    mempool.AddTransactionsUpdated(1);
    // C++的条件变量，唤醒所有阻塞的线程，告诉它们以更新链的Tip块；
    cvBlockChange.notify_all();

    static bool fWarned = false;
    std::vector<std::string> warningMessages;
    //非初始化下载
    if (!IsInitialBlockDownload()) {
        int nUpgraded = 0;
        const CBlockIndex *pindex = chainActive.Tip();
        // 查看该当前设置的Tip块上是否有未激活的 新功能。
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(
                pindex, chainParams.GetConsensus(), warningcache[bit]);
            if (state == THRESHOLD_ACTIVE || state == THRESHOLD_LOCKED_IN) {
                if (state == THRESHOLD_ACTIVE) {
                    std::string strWarning =
                        strprintf(_("Warning: unknown new rules activated "
                                    "(versionbit %i)"),
                                  bit);
                    SetMiscWarning(strWarning);
                    if (!fWarned) {
                        AlertNotify(strWarning);
                        fWarned = true;
                    }
                } else {
                    warningMessages.push_back(
                        strprintf("unknown new rules are about to activate "
                                  "(versionbit %i)",
                                  bit));
                }
            }
        }
        // Check the version of the last 100 blocks to see if we need to
        // upgrade: 检查最近100个块的版本字节，查看是否需要进行更新软件版本
        // (因为现在是使用版本号作为软件的更新方式)
        for (int i = 0; i < 100 && pindex != nullptr; i++) {
            int32_t nExpectedVersion =
                ComputeBlockVersion(pindex->pprev, chainParams.GetConsensus());
            if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION &&
                (pindex->nVersion & ~nExpectedVersion) != 0)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        //
        if (nUpgraded > 0)
            warningMessages.push_back(strprintf(
                "%d of last 100 blocks have unexpected version", nUpgraded));
        if (nUpgraded > 100 / 2) {
            std::string strWarning =
                _("Warning: Unknown block versions being mined! It's possible "
                  "unknown rules are in effect");
            // notify GetWarnings(), called by Qt and the JSON-RPC code to warn
            // the user:
            SetMiscWarning(strWarning);
            if (!fWarned) {
                AlertNotify(strWarning);
                fWarned = true;
            }
        }
    }
    LogPrintf(
        "%s: new best=%s height=%d version=0x%08x log2_work=%.8g tx=%lu "
        "date='%s' progress=%f cache=%.1fMiB(%utxo)",
        __func__, chainActive.Tip()->GetBlockHash().ToString(),
        chainActive.Height(), chainActive.Tip()->nVersion,
        log(chainActive.Tip()->nChainWork.getdouble()) / log(2.0),
        (unsigned long)chainActive.Tip()->nChainTx,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
                          chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainParams.TxData(), chainActive.Tip()),
        pcoinsTip->DynamicMemoryUsage() * (1.0 / (1 << 20)),
        pcoinsTip->GetCacheSize());
    if (!warningMessages.empty())
        LogPrintf(" warning='%s'",
                  boost::algorithm::join(warningMessages, ", "));
    LogPrintf("\n");
}

/**
 * Disconnect chainActive's tip. You probably want to call
 * mempool.removeForReorg and manually re-limit mempool size after this, with
 * cs_main held.
 * 拒绝当前链的Tip块。可能会调用removeForReorg，并且手动限制交易池的带下
 */
static bool DisconnectTip(const Config &config, CValidationState &state,
                          bool fBare = false) {
    const Consensus::Params &consensusParams =
        config.GetChainParams().GetConsensus();
    //1. 获取当前链的Tip块
    CBlockIndex *pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    CBlock block;
    //2. 从磁盘读取该块的信息
    if (!ReadBlockFromDisk(block, pindexDelete, consensusParams)) {
        return AbortNode(state, "Failed to read block");
    }

    // Apply the block atomically to the chain state.
    //3. 以原子方式将块用于 链状态。
    int64_t nStart = GetTimeMicros();
    {
        //4. 获取全局UTXO集合的视角
        CCoinsViewCache view(pcoinsTip);
        //5. 拒绝该区块
        if (DisconnectBlock(block, pindexDelete, view) != DISCONNECT_OK) {
            return error("DisconnectTip(): DisconnectBlock %s failed",
                         pindexDelete->GetBlockHash().ToString());
        }

        bool flushed = view.Flush();
        assert(flushed);
    }

    LogPrint("bench", "- Disconnect block: %.2fms\n",
             (GetTimeMicros() - nStart) * 0.001);

    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED)) {
        return false;
    }

    if (!fBare) {
        // Resurrect mempool transactions from the disconnected block.
        std::vector<uint256> vHashUpdate;
        for (const auto &it : block.vtx) {
            const CTransaction &tx = *it;
            // ignore validation errors in resurrected transactions
            CValidationState stateDummy;
            if (tx.IsCoinBase() ||
                !AcceptToMemoryPool(config, mempool, stateDummy, it, false,
                                    nullptr, nullptr, true)) {
                mempool.removeRecursive(tx, MemPoolRemovalReason::REORG);
            } else if (mempool.exists(tx.GetId())) {
                vHashUpdate.push_back(tx.GetId());
            }
        }
        // AcceptToMemoryPool/addUnchecked all assume that new mempool entries
        // have no in-mempool children, which is generally not true when adding
        // previously-confirmed transactions back to the mempool.
        // UpdateTransactionsFromBlock finds descendants of any transactions in
        // this block that were added back and cleans up the mempool state.
        // 查找block中交易的所有后代交易，重新添加进mempool，并清理mempool中的状态。
        mempool.UpdateTransactionsFromBlock(vHashUpdate);
    }

    // Update chainActive and related variables.
    UpdateTip(config, pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    for (const auto &tx : block.vtx) {
        GetMainSignals().SyncTransaction(
            *tx, pindexDelete->pprev,
            CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

/**
 * Used to track blocks whose transactions were applied to the UTXO state as a
 * part of a single ActivateBestChainStep call.
 * 用来跟踪把交易放入UTXO集合中的块; 作为激活链调用的一部分
 */
struct ConnectTrace {
    std::vector<std::pair<CBlockIndex *, std::shared_ptr<const CBlock>>>
        blocksConnected;
};

/**
 * Connect a new block to chainActive. pblock is either nullptr or a pointer to
 * a CBlock corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is always added to connectTrace (either after loading from disk or
 * by copying pblock) - if that is not intended, care must be taken to remove
 * the last entry in blocksConnected in case of failure.
 * 接收一个新的区块到主链。pblock指向一个nil或指向一个pindexNew对应的block,以便绕过从硬盘加载它。
 * 该块被一直添加到connectTrace(当从磁盘加载或复制pblock之后)，-- 如果不是明确要求，
 * 一旦失败，必须移除blocksConnected 的最后一项
 * pindexNew(in):当前链接的块的索引；pblock(in):链接的区块数据，有可能为nil.
 * connectTrace(out):
 */
static bool ConnectTip(const Config &config, CValidationState &state,
                       CBlockIndex *pindexNew,
                       const std::shared_ptr<const CBlock> &pblock,
                       ConnectTrace &connectTrace
) {
    const CChainParams &chainparams = config.GetChainParams();
    //注意：链接到链上的块的父块必须在激活链的Tip上(只有这样的块才可以链接到链上)
    assert(pindexNew->pprev == chainActive.Tip());
    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    //1. 如果块的指针为空，就从磁盘上读取该数据，并将该块数据和它对应的索引加入 connectTrace 尾部。
    if (!pblock) {
        // 做一个空的共享指针
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        // 将该块的索引，以及块数据的指针添加进跟踪结构
        connectTrace.blocksConnected.emplace_back(pindexNew, pblockNew);
        // 从磁盘中读取该块的数据
        if (!ReadBlockFromDisk(*pblockNew, pindexNew,
                               chainparams.GetConsensus()))
            return AbortNode(state, "Failed to read block");
    } else {
        // 将该块的索引，以及块数据的指针添加进跟踪结构
        connectTrace.blocksConnected.emplace_back(pindexNew, pblock);
    }

    //2. 拿到将要链接的块的数据
    const CBlock &blockConnecting = *connectTrace.blocksConnected.back().second;
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n",
             (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        CCoinsViewCache view(pcoinsTip);
        // 链接区块
        bool rv = ConnectBlock(config, blockConnecting, state, pindexNew, view,
                               chainparams);
        // 发送信号，标识块链接的情况
        GetMainSignals().BlockChecked(blockConnecting, state);
        // 如果块链接失败，标记状态，并返回
        if (!rv) {
            if (state.IsInvalid()) {
                InvalidBlockFound(pindexNew, state);
            }
            return error("ConnectTip(): ConnectBlock %s failed",
                         pindexNew->GetBlockHash().ToString());
        }
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n",
                 (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        bool flushed = view.Flush();
        assert(flushed);
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001,
             nTimeFlush * 0.000001);
    // Write the chain state to disk, if necessary.
    // 新块成功链接到块链上， 然后将块链的状态写入磁盘。
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED)) return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n",
             (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    // Remove conflicting transactions from the mempool.;
    // 从交易池中移除该链接块上的所有交易；(因为这些交易已被打包)
    mempool.removeForBlock(blockConnecting.vtx, pindexNew->nHeight);
    // Update chainActive & related variables.
    // 更新链的顶端，
    UpdateTip(config, pindexNew);

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n",
             (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n",
             (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't known to be
 * invalid (it's however far from certain to be valid).
 * 从候选集合中
 */
static CBlockIndex *FindMostWorkChain() {
    do {
        CBlockIndex *pindexNew = nullptr;

        // Find the best candidate header.
        {
            // 查询候选集合，获取它最后一个元素的数据
            std::set<CBlockIndex *, CBlockIndexWorkComparator>::reverse_iterator
                it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend()) return nullptr;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active
        // chain and the candidate are valid. Just going until the active chain
        // is an optimization, as we know all blocks in it are valid already.
        // 检查在这条激活链至候选区块之间所有的 块是否有效。仅跑在所有的区块是有效的链上。
        CBlockIndex *pindexTest = pindexNew;
        bool fInvalidAncestor = false;

        // 存在候选区块，并且该区块不在当前链中，进入下列循环
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted. Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            // 裁剪节点可能在  setBlockIndexCandidates 中含有删除块文件的区块。从候选集合中删除这些数据，
            // 只有当我们有一个 non-active-chain 的所有父区块时，才可以切换最长链至它上。
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            // 当该块所引在一条失败的链， 或丢失块数据时，进入下列条件
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing
                // data)
                // 当在一条失败的链，且该块的工作量大于 全局状态缓存的无效块时，更新全局无效变量。
                if (fFailedChain &&
                    (pindexBestInvalid == nullptr ||
                     pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex *pindexFailed = pindexNew;
                // Remove the entire chain from the set.  从候选集合中，移除该区块
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to
                        // mapBlocksUnlinked, so that if the block arrives in
                        // the future we can try adding to
                        // setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(
                            std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }

            // 继续查看它的父区块，直到父区块为空，或者父区块在当前激活链中找到，标识此块以及它的所有父区块都可以链接到主链
            pindexTest = pindexTest->pprev;
        }

        // 当候选集合中某个元素没有无效的祖先时，即该区块为当前的最大工作量；
        // 当候选集合中的某个元素有无效祖先，将该元素从 候选集合中 删除，并继续查询集合中 的其它元素
        if (!fInvalidAncestor) return pindexNew;
    } while (true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the
 * current tip. */
static void PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to
    // return to it later in case a reorganization to a better block fails.
    std::set<CBlockIndex *, CBlockIndexWorkComparator>::iterator it =
        setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() &&
           setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left
    // in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either nullptr or a pointer to a CBlock corresponding to
 * pindexMostWork.
 * 尝试做一些处理，将 pindexMostWork 作为主链的Tip块。 pblock 是nil或者指向pindexMostWork对应的区块
 * pindexMostWork(in):当前所有链的最大工作量；pblock(in):用来设置最长链的块
 * fInvalidFound(out):传出参数，是否找到无效的区块。connectTrace(out):传出参数，存储将要跟踪的区块。
 */
static bool ActivateBestChainStep(const Config &config, CValidationState &state,
                                  CBlockIndex *pindexMostWork,
                                  const std::shared_ptr<const CBlock> &pblock,
                                  bool &fInvalidFound,
                                  ConnectTrace &connectTrace) {
    AssertLockHeld(cs_main);
    //1. 找到当前链的Tip； 以及当前链与最大工作量之间的分叉点。
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    //2. 拒绝不在不在最长链上的激活块； 就是将 当前链的分叉点 到Tip块之间的区块都拒绝掉。
    bool fBlocksDisconnected = false;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(config, state)) return false;
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.  构建一系列块，链接到主链上。
    std::vector<CBlockIndex *> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the
        // best tip, as we likely only need a few blocks along the way.
        // 每轮最多链接32个区块
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;        //设置到当前链的最大高度

        // Connect new blocks. 遍历上步收集的所有有效块的集合
        for (CBlockIndex *pindexConnect :
             boost::adaptors::reverse(vpindexToConnect)) {
            // pindexConnect
            if (!ConnectTip(config, state, pindexConnect,
                            pindexConnect == pindexMostWork
                                ? pblock
                                : std::shared_ptr<const CBlock>(),
                            connectTrace)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.  block违反了共识规则
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back());
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    // If we didn't actually connect the block, don't notify
                    // listeners about it
                    connectTrace.blocksConnected.pop_back();
                    break;
                } else {
                    // A system error occurred (disk space, database error,
                    // ...).
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip ||
                    chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return
                    // temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1,
                               STANDARD_LOCKTIME_VERIFY_FLAGS);
        LimitMempoolSize(
            mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
            GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
    }
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

//发送信号，接受到链的Tip块
static void NotifyHeaderTip() {
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex *pindexHeaderOld = nullptr;      //函数中的静态变量，相当于存储上次的Tip块。
    CBlockIndex *pindexHeader = nullptr;
    {
        LOCK(cs_main);
        //1. 获取当前的Tip块
        pindexHeader = pindexBestHeader;

        //2. 当前链的Tip块与上次存储的不一样时， 发送信号通知 。
        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            pindexHeaderOld = pindexHeader;             //缓存当前的Tip块到静态变量
        }
    }
    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either nullptr or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 * 激活最长链
 *  pblock(in):刚接收的区块； state(out):检测的状态；
 */
bool ActivateBestChain(const Config &config, CValidationState &state,
                       std::shared_ptr<const CBlock> pblock) {
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    // 注意：虽然我们经常从 ProcessNewBlock 调用它。但这远没有保证。因为P2P/RPC经常会
    // 给我们传递额外的信息，所以不要假设 pblock 是为高性能或正确性设置的。

    CBlockIndex *pindexMostWork = nullptr;
    CBlockIndex *pindexNewTip = nullptr;
    do {
        boost::this_thread::interruption_point();      //检测当前的线程是否已被终端
        if (ShutdownRequested()) break;

        const CBlockIndex *pindexFork;
        ConnectTrace connectTrace;
        bool fInitialDownload;
        {
            LOCK(cs_main);
            {
                // TODO: Tempoarily ensure that mempool removals are notified
                // before connected transactions. This shouldn't matter, but the
                // abandoned state of transactions in our wallet is currently
                // cleared when we receive another notification and there is a
                // race condition where notification of a connected conflict
                // might cause an outside process to abandon a transaction and
                // then have it inadvertantly cleared by the notification that
                // the conflicted transaction was evicted.
                // 临时保证在关联交易前通知mempool清除。这不重要，但是当我们接收到其他的
                // 通知时，在钱包中废弃的交易状态应该被清除；并且此处有一个竞态条件，链接冲突的消息
                // 可能会导致一个额外的进程去废弃该交易，然后在不经意间依据通知清除这个冲突的交易。

                MemPoolConflictRemovalTracker mrt(mempool);     //构造一个交易池跟踪器
                CBlockIndex *pindexOldTip = chainActive.Tip();
                //1. 获取当前所有块的最大工作量
                if (pindexMostWork == nullptr) {
                    pindexMostWork = FindMostWorkChain();
                }

                //2. 标识最长链 的Tip已为最大工作量
                // Whether we have anything to do at all.
                if (pindexMostWork == nullptr ||
                    pindexMostWork == chainActive.Tip())
                    return true;

                bool fInvalidFound = false;
                std::shared_ptr<const CBlock> nullBlockPtr;
                //如果最大工作量等于传入的区块，就将其传入；否则，传入一个空对象。
                if (!ActivateBestChainStep(
                        config, state, pindexMostWork,
                        pblock &&
                                pblock->GetHash() ==
                                    pindexMostWork->GetBlockHash()
                            ? pblock
                            : nullBlockPtr,
                        fInvalidFound, connectTrace))
                    return false;
                // 发现无效的区块
                if (fInvalidFound) {
                    // Wipe cache, we may need another branch now. 此时可能需要采用另一条链
                    pindexMostWork = nullptr;
                }
                pindexNewTip = chainActive.Tip();
                pindexFork = chainActive.FindFork(pindexOldTip);    //查找当前主链到块的分叉点
                fInitialDownload = IsInitialBlockDownload();

                // throw all transactions though the signal-interface

            } // MemPoolConflictRemovalTracker destroyed and conflict evictions
              // are notified  通知MemPoolConflictRemovalTracker 销毁和冲突驱除

            // Transactions in the connnected block are notified  在接收区块中的交易被通知
            for (const auto &pair : connectTrace.blocksConnected) {
                assert(pair.second);
                const CBlock &block = *(pair.second);
                for (unsigned int i = 0; i < block.vtx.size(); i++)
                    GetMainSignals().SyncTransaction(*block.vtx[i], pair.first,
                                                     i);
            }
        }
        // When we reach this point, we switched to a new tip (stored in
        // pindexNewTip).
        // 当到这步，当前链已切换到一个新的Tip(存储在pindexNewTip 中)
        // Notifications/callbacks that can run without cs_main
        // 不加锁，执行信号槽函数
        // Notify external listeners about the new tip.
        // 通知其他的监听者，更新新高度了。

        GetMainSignals().UpdatedBlockTip(pindexNewTip, pindexFork,
                                         fInitialDownload);

        // Always notify the UI if a new block tip was connected； 通知UI，一个新的块Tip被链接到
        if (pindexFork != pindexNewTip) {
            uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
        }
    } while (pindexNewTip != pindexMostWork);
    // 检查全局的块索引
    CheckBlockIndex(config.GetChainParams().GetConsensus());

    // Write changes periodically to disk, after relay. 在中继信息之后，定期写数据变化到磁盘。
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool PreciousBlock(const Config &config, CValidationState &state,
                   CBlockIndex *pindex) {
    {
        LOCK(cs_main);
        //如果该块索引的工作量 < 激活链Tip的工作量；
        if (pindex->nChainWork < chainActive.Tip()->nChainWork) {
            // Nothing to do, this block is not at the tip.
            return true;
        }
        // 激活链的工作量大于 全局状态
        if (chainActive.Tip()->nChainWork > nLastPreciousChainwork) {
            // The chain has been extended since the last call, reset the
            // counter.
            nBlockReverseSequenceId = -1;
        }
        // 重新对全局状态赋值
        nLastPreciousChainwork = chainActive.Tip()->nChainWork;
        // 从全局的候选集合中删除该索引
        setBlockIndexCandidates.erase(pindex);
        // 对index的状态赋值
        pindex->nSequenceId = nBlockReverseSequenceId;
        // 循环使用该计数器
        if (nBlockReverseSequenceId > std::numeric_limits<int32_t>::min()) {
            // We can't keep reducing the counter if somebody really wants to
            // call preciousblock 2**31-1 times on the same set of tips...
            nBlockReverseSequenceId--;
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && pindex->nChainTx) {
            setBlockIndexCandidates.insert(pindex);
            PruneBlockIndexCandidates();
        }
    }

    return ActivateBestChain(config, state);
}

bool InvalidateBlock(const Config &config, CValidationState &state,
                     CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.  表示一个块为无效
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    // 从激活链上 拒绝掉包含该交易后的 区块
    while (chainActive.Contains(pindex)) {
        CBlockIndex *pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        // 拒绝掉激活链的 顶端块
        if (!DisconnectTip(config, state)) {
            mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1,
                                   STANDARD_LOCKTIME_VERIFY_FLAGS);
            return false;
        }
    }
    //限制交易池的大小
    LimitMempoolSize(
        mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
        GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore,
    // so add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) &&
            it->second->nChainTx &&
            !setBlockIndexCandidates.value_comp()(it->second,
                                                  chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1,
                           STANDARD_LOCKTIME_VERIFY_FLAGS);
    uiInterface.NotifyBlockTip(IsInitialBlockDownload(), pindex->pprev);
    return true;
}

bool ResetBlockFailureFlags(CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() &&
            it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) &&
                it->second->nChainTx &&
                setBlockIndexCandidates.value_comp()(chainActive.Tip(),
                                                     it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of
                // those.
                pindexBestInvalid = nullptr;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != nullptr) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

// 添加该块的索引至全局状态；setDirtyBlockIndex，mapBlockIndex ，pindexBestHeader， 并更新创建的块索引的内部数据
// block(in):添加的区块；
CBlockIndex *AddToBlockIndex(const CBlockHeader &block) {
    //1. Check for duplicate；如果该区块已经在全局状态中，直接返回它已有的索引； 避免重复添加
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end()) return it->second;

    //2. Construct new block index object； 构造新快的索引
    CBlockIndex *pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    //3. 只有当块的所有数据都接收到时，才给该块的索引分配 sequenceID。避免矿工只广播块头，而扣押该块的交易数据，造成不正当的竞争。
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    //4. 为创建的块索引修改状态，并将它加入全局状态中
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi =
        mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    //5. 在全局状态中查找该块的父区块；并继续更改新建块索引的状态
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    //6. 更新当前链的最大时间和工作量(因为该块为当前链的Tip)
    pindexNew->nTimeMax =
        (pindexNew->pprev
             ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime)
             : pindexNew->nTime);
    pindexNew->nChainWork =
        (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) +
        GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    //7. 更新全局状态的Tip为该块。
    if (pindexBestHeader == nullptr ||
        pindexBestHeader->nChainWork < pindexNew->nChainWork) {
        pindexBestHeader = pindexNew;
    }
    //8. 将该块的索引加入全局状态中
    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/**
 * Mark a block as having its data received and checked (up to
 * BLOCK_VALID_TRANSACTIONS).
 * 表识已接收到一个区块的 交易信息；所以修改块索引的状态;提升块至BLOCK_VALID_TRANSACTIONS，BLOCK_HAVE_DATA。
 * 同时，将该块索引放入全局的 Tip备选状态中setBlockIndexCandidates，或者放入全局的孤儿状态中mapBlocksUnlinked。
 * pindexNew(in And Out)参一块的索引(即接收该区块的交易数据); block(in); state(out); pos(in)
 */
bool ReceivedBlockTransactions(const CBlock &block, CValidationState &state,
                               CBlockIndex *pindexNew,
                               const CDiskBlockPos &pos) {
    //1. 对pindexNew 的部分数据进行赋值
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);     //提升块的等级
    setDirtyBlockIndex.insert(pindexNew);

    //2. 当pindexNew 是创世块，或者该区块的所有父区块的交易都已经收到，进入下列条件
    if (pindexNew->pprev == nullptr || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are
        // BLOCK_VALID_TRANSACTIONS.
        // 临时集合，添加区块索引；存储该块，以及他所有的后代区块；这些区块中符合条件的(即
        // 工作量大于当前激活链的Tip时)，将所有符合条件的块索引都放入全局 Tip候选状态中。
        std::deque<CBlockIndex *> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to
        // be connected.
        //递归处理 该区块以及当前它所有合格的后代区块，进入该循环
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();    //处理当前区块的索引
            queue.pop_front();
            // 修改索引中关于链交易数的状态
            pindex->nChainTx =
                (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                //修改sequenceID的状态
                pindex->nSequenceId = nBlockSequenceId++;
            }
            // 如果 当前块的工作量大于最顶端块，或者Tip 块为空，将当前的块索引放入备选的Tip集合中
            if (chainActive.Tip() == nullptr ||
                !setBlockIndexCandidates.value_comp()(pindex,
                                                      chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }

            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                range = mapBlocksUnlinked.equal_range(pindex);
            // 查找该区块是 否有后代区块作为孤块存在于 全局状态中； 有的话，进入下列循环，将它所有的后代区块都加入临时集合中，
            // 然后从此全局状态中， 删除该块索引(标识 没有以该该块作为父块的孤块了)
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator it =
                    range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        // 标识该块时个孤块， 将该区块作为孤块放入全局状态中； 即该区块存在父块，并且父块在链中，
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(
                std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

// 查找将写入该区块的文件，并对pos 进行。(主要是查看磁盘是否可以承载接下来块的数据)；可以TRUE，否则false。
// state(out):状态；pos(in/out)块文件在磁盘中的位置；nAddSize(in):写入文件的大小(块全部的大小+8字节)
// nHeight(in):该区块的高度；nTime(in):该区块的时间; fKnown(in):是否知道该文件的位置；如果此处为false，
// 则参二就是一个CDiskBlockPos的空对象；否则是一个含有数据的对象
bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos,
                  unsigned int nAddSize, unsigned int nHeight, uint64_t nTime,
                  bool fKnown = false) {
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;

    //缓存所有的文件信息()
    if (vinfoBlockFile.size() <= nFile) {
        //全局状态的容量达到阈值，进行向后扩容
        vinfoBlockFile.resize(nFile + 1);
    }

    // 如果事先不知道 文件的位置
    if (!fKnown) {
        // 判断当前的文件是否可以存储下该区块的数据， 否的话，将文件号向后移动(即创建新文件)
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    // nFile 此时标识，区块数据将要写入的文件号，当它与全局状态的最后一个文件不等时，
    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile,
                      vinfoBlockFile[nLastBlockFile].ToString());
        }
        // 更新文件的状态
        FlushBlockFile(!fKnown);
        // 更新全局状态至最新文件
        nLastBlockFile = nFile;
    }
    //更新文件结构中的信息；(只更新文件信息中的块号和时间)
    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    //更新文件中的字节信息。
    if (fKnown)
        vinfoBlockFile[nFile].nSize =
            std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    //如果不知道文件
    if (!fKnown) {
        unsigned int nOldChunks =
            (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks =
            (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) /
            BLOCKFILE_CHUNK_SIZE;

        if (nNewChunks > nOldChunks) {
            if (fPruneMode) fCheckForPruning = true;        //在裁剪模式下，标识可以分配更多的文件空间
            // 检测磁盘是否含有如此多的内存空间(需要些的数据大小+额外分配的字节大小)
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                //打开文件(此时文件描述符处于一个位置)
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf(
                        "Pre-allocating up to position 0x%x in blk%05u.dat\n",
                        nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    //从pos位置开始，向文件中写入 参数三大小的假数据；
                    AllocateFileRange(file, pos.nPos,
                                      nNewChunks * BLOCKFILE_CHUNK_SIZE -
                                          pos.nPos);
                    fclose(file);
                }
            } else
                return state.Error("out of disk space");
        }
    }
    // 脏文件
    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos,
                 unsigned int nAddSize) {
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks =
        (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks =
        (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode) fCheckForPruning = true;
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n",
                          nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos,
                                  nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        } else
            return state.Error("out of disk space");
    }

    return true;
}

bool CheckBlockHeader(const CBlockHeader &block, CValidationState &state,
                      const Consensus::Params &consensusParams,
                      bool fCheckPOW) {
    // Check proof of work matches claimed amount
    if (fCheckPOW &&
        !CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
        return state.DoS(50, false, REJECT_INVALID, "high-hash", false,
                         "proof of work failed");

    return true;
}

//检查块；block(in) 将要检查的块； state(out):检查后块的状态
bool CheckBlock(const Config &config, const CBlock &block,
                CValidationState &state,
                const Consensus::Params &consensusParams, bool fCheckPOW,
                bool fCheckMerkleRoot) {
    // These are checks that are independent of context.
    if (block.fChecked) {
        return true;
    }

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, consensusParams, fCheckPOW)) {
        return false;
    }

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot",
                             true, "hashMerkleRoot mismatch");
        }

        // Check for merkle tree malleability (CVE-2012-2459): repeating
        // sequences of transactions in a block without affecting the merkle
        // root of a block, while still invalidating it.
        if (mutated) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate",
                             true, "duplicate transaction");
        }
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.
    // 所有潜在的错误验证必须在 交易验证之前做，这样就可以标识这个块头为无效，因为接收的这个块中包含无效的交易

    // First transaction must be coinbase.
    if (block.vtx.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false,
                         "first tx is not coinbase");
    }

    // Size limits.
    auto nMaxBlockSize = config.GetMaxBlockSize();

    // Bail early if there is no way this block is of reasonable size.
    if ((block.vtx.size() * MIN_TRANSACTION_SIZE) > nMaxBlockSize) {
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false,
                         "size limits failed");
    }

    auto currentBlockSize =
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    if (currentBlockSize > nMaxBlockSize) {
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false,
                         "size limits failed");
    }

    // And a valid coinbase.
    if (!CheckCoinbase(*block.vtx[0], state, false)) {
        return state.Invalid(false, state.GetRejectCode(),
                             state.GetRejectReason(),
                             strprintf("Coinbase check failed (txid %s) %s",
                                       block.vtx[0]->GetId().ToString(),
                                       state.GetDebugMessage()));
    }

    // Keep track of the sigops count.
    uint64_t nSigOps = 0;
    auto nMaxSigOpsCount = GetMaxBlockSigOpsCount(currentBlockSize);

    // Check transactions
    auto txCount = block.vtx.size();
    auto *tx = block.vtx[0].get();

    size_t i = 0;
    while (true) {
        // Count the sigops for the current transaction. If the total sigops
        // count is too high, the the block is invalid.
        nSigOps += GetSigOpCountWithoutP2SH(*tx);
        if (nSigOps > nMaxSigOpsCount) {
            return state.DoS(100, false, REJECT_INVALID, "bad-blk-sigops",
                             false, "out-of-bounds SigOpCount");
        }

        // Go to the next transaction.
        i++;

        // We reached the end of the block, success.
        if (i >= txCount) {
            break;
        }

        // Check that the transaction is valid. because this check differs for
        // the coinbase, the loos is arranged such as this only runs after at
        // least one increment.
        tx = block.vtx[i].get();
        if (!CheckRegularTransaction(*tx, state, false)) {
            return state.Invalid(
                false, state.GetRejectCode(), state.GetRejectReason(),
                strprintf("Transaction check failed (txid %s) %s",
                          tx->GetId().ToString(), state.GetDebugMessage()));
        }
    }

    if (fCheckPOW && fCheckMerkleRoot) {
        block.fChecked = true;
    }

    return true;
}

// pindexPrev(in):参四的父区块索引；state(out):处理中的状态；
// hash(in):检查的块哈希，未使用；
// 检查接收的块是否为检查点进行分叉的块
static bool CheckIndexAgainstCheckpoint(const CBlockIndex *pindexPrev,
                                        CValidationState &state,
                                        const CChainParams &chainparams,
                                      const uint256 &hash) {
    //1. 如果该块为创世块，则返回TRUE。
    if (*pindexPrev->phashBlock ==
        chainparams.GetConsensus().hashGenesisBlock) {
        return true;
    }

    //2. 获取检查块的高度
    int nHeight = pindexPrev->nHeight + 1;
    // Don't accept any forks from the main chain prior to last checkpoint
    // 在这些检查点之前的块，不接受任何来自主链的分叉。(因为这些检查点都是以前主链上稳定的区块)
    CBlockIndex *pcheckpoint =
        Checkpoints::GetLastCheckpoint(chainparams.Checkpoints());
    if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
        return state.DoS(
            100,
            error("%s: forked chain older than last checkpoint (height %d)",
                  __func__, nHeight));
    }

    return true;
}

// 区块头的上下文检查； 1. 检查工作量，2.检查块的时间，3.检查块的版本(依据父区块检查该块头的状态，父区块此时可以在全局变量中找到)
// block(in):检查的块； state(out):检查的状态；pindexPrev(in):检查块的父区块； nAdjustedTime(in):时间,当前的系统时间+用户自定义的偏移时间
bool ContextualCheckBlockHeader(const CBlockHeader &block,
                                CValidationState &state,
                                const Consensus::Params &consensusParams,
                                const CBlockIndex *pindexPrev,
                                int64_t nAdjustedTime) {
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    //1. Check proof of work；检查工作量
    if (block.nBits !=
        GetNextWorkRequired(pindexPrev, &block, consensusParams)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-diffbits", false,
                         "incorrect proof of work");
    }

    //2. Check timestamp against prev； 检查当前接受块的时间戳
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast()) {
        return state.Invalid(false, REJECT_INVALID, "time-too-old",
                             "block's timestamp is too early");
    }

    // Check timestamp
    if (block.GetBlockTime() > nAdjustedTime + 2 * 60 * 60) {
        return state.Invalid(false, REJECT_INVALID, "time-too-new",
                             "block timestamp too far in the future");
    }

    // Reject outdated version blocks when 95% (75% on testnet) of the network
    // has upgraded:
    //3. check for version 2, 3 and 4 upgrades； 检查块的版本号设置
    if ((block.nVersion < 2 && nHeight >= consensusParams.BIP34Height) ||
        (block.nVersion < 3 && nHeight >= consensusParams.BIP66Height) ||
        (block.nVersion < 4 && nHeight >= consensusParams.BIP65Height)) {
        return state.Invalid(
            false, REJECT_OBSOLETE,
            strprintf("bad-version(0x%08x)", block.nVersion),
            strprintf("rejected nVersion=0x%08x block", block.nVersion));
    }

    return true;
}

//检查交易； tx(in):检查的交易； state(out):检查的状态；nHeight(in):该交易所在块高度；
//nLockTimeCutoff(in):该交易所在的块的时间；
bool ContextualCheckTransaction(const Config &config, const CTransaction &tx,
                                CValidationState &state,
                                const Consensus::Params &consensusParams,
                                int nHeight, int64_t nLockTimeCutoff) {
    //1. 检查交易的状态;是否为final
    if (!IsFinalTx(tx, nHeight, nLockTimeCutoff)) {
        // While this is only one transaction, we use txns in the error to
        // ensure continuity with other clients.
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false,
                         "non-final transaction");
    }
    //2. 是否为UAHF后的交易
    if (IsUAHFenabled(config, nHeight) &&
        nHeight <= consensusParams.antiReplayOpReturnSunsetHeight) {
        for (const CTxOut &o : tx.vout) {
            if (o.scriptPubKey.IsCommitment(
                    consensusParams.antiReplayOpReturnCommitment)) {
                return state.DoS(10, false, REJECT_INVALID, "bad-txn-replay",
                                 false, "non playable transaction");
            }
        }
    }

    return true;
}

//依据当前区块的上下文环境下进行交易 检查；查看是否为final交易(既可以打包)，是否符合UAHF规则的交易
//tx(in):检查的交易；state(out):检查的状态
bool ContextualCheckTransactionForCurrentBlock(
    const Config &config, const CTransaction &tx, CValidationState &state,
    const Consensus::Params &consensusParams, int flags) {
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the current
    // network-enforced consensus rules should be used. In a future soft-fork
    // scenario that would mean checking which rules would be enforced for the
    // next block and setting the appropriate flags. At the present time no
    // soft-forks are scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // ContextualCheckTransactionForCurrentBlock() uses chainActive.Height()+1
    // to evaluate nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being* evaluated is what
    // is used. Thus if we want to know if a transaction can be part of the
    // *next* block, we need to call ContextualCheckTransaction() with one more
    // than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // ContextualCheckTransaction() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nLockTimeCutoff = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                                        ? chainActive.Tip()->GetMedianTimePast()
                                        : GetAdjustedTime();

    return ContextualCheckTransaction(config, tx, state, consensusParams,
                                      nBlockHeight, nLockTimeCutoff);
}

//检查块的上下文；block(in):检查的块；state(out):检查后的状态；pindexPrev(in):检查块的父区块
// 根据交易所在的块高度，采用不同的规则，检查块中的交易格式。
bool ContextualCheckBlock(const Config &config, const CBlock &block,
                          CValidationState &state,
                          const Consensus::Params &consensusParams,
                          const CBlockIndex *pindexPrev) {
    //1. 获取当前块的高度
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    //2. 版本号检测
    if (VersionBitsState(pindexPrev, consensusParams, Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
    }

    //3. 获取块的中值时间
    const int64_t nMedianTimePast =
        pindexPrev == nullptr ? 0 : pindexPrev->GetMedianTimePast();

    const int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                                        ? nMedianTimePast
                                        : block.GetBlockTime();

    // Check that all transactions are finalized
    //4. 检查块的交易
    for (const auto &tx : block.vtx) {
        if (!ContextualCheckTransaction(config, *tx, state, consensusParams,
                                        nHeight, nLockTimeCutoff)) {
            // state set by ContextualCheckTransaction.
            return false;
        }
    }

    // Enforce rule that the coinbase starts with serialized block height
    //5. 强制coinbase以高度字节的序列化开始
    if (nHeight >= consensusParams.BIP34Height) {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(),
                        block.vtx[0]->vin[0].scriptSig.begin())) {
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false,
                             "block height mismatch in coinbase");
        }
    }

    return true;
}

//接收块头 block(in): 将要接收的块； state(out): 处理过程中的状态；
//ppindex(out): 创建接收块的索引(即参一)
// 每当接收到一个块时，都会先检查块，符合条件，创建该块的索引，并更新其索引的状态，
//然后将该索引加入全局状态中，然后检查全局状态中的所有索引。
static bool AcceptBlockHeader(const Config &config, const CBlockHeader &block,
                              CValidationState &state, CBlockIndex **ppindex) {
    AssertLockHeld(cs_main);
    const CChainParams &chainparams = config.GetChainParams();

    // Check for duplicate
    uint256 hash = block.GetHash();
    //1. 查找该区块在全局状态中是否存在
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex *pindex = nullptr;
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {
        //2. 存在，判断该块的状态，打印log，然后退出。
        if (miSelf != mapBlockIndex.end()) {
            // Block header is already known.
            pindex = miSelf->second;
            if (ppindex) {
                *ppindex = pindex;
            }
            if (pindex->nStatus & BLOCK_FAILED_MASK) {
                return state.Invalid(error("%s: block %s is marked invalid",
                                           __func__, hash.ToString()),
                                     0, "duplicate");
            }
            return true;
        }

        //3. 不存在于全局状态中，检查块的工作量Pow
        if (!CheckBlockHeader(block, state, chainparams.GetConsensus())) {
            return error("%s: Consensus::CheckBlockHeader: %s, %s", __func__,
                         hash.ToString(), FormatStateMessage(state));
        }

        //4. Get prev block index； 查看该块的父区块是否在全局状态中，不存在，出错，退出
        CBlockIndex *pindexPrev = nullptr;
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end()) {
            return state.DoS(10, error("%s: prev block not found", __func__), 0,
                             "bad-prevblk");
        }

        //5. 获取块的父区块索引； 查看块的父区块状态，出错退出
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            return state.DoS(100, error("%s: prev block invalid", __func__),
                             REJECT_INVALID, "bad-prevblk");
        }

        assert(pindexPrev);
        //6. 检查一个块，是否来自在检查点前的分叉块
        if (fCheckpointsEnabled &&
            !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams,
                                         hash)) {
            return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__,
                         state.GetRejectReason().c_str());
        }

        //7. 块的上下文检查；
        if (!ContextualCheckBlockHeader(block, state,
                                        chainparams.GetConsensus(), pindexPrev,
                                        GetAdjustedTime())) {
            return error("%s: Consensus::ContextualCheckBlockHeader: %s, %s",
                         __func__, hash.ToString(), FormatStateMessage(state));
        }
    }
    //4. 如果pindex为NULL，标识该块上述检查已通过，且它的父区块在全局状态中可以找到。接下来将该块加入全局状态中。
    if (pindex == nullptr) {
        pindex = AddToBlockIndex(block);
    }

    if (ppindex) {
        *ppindex = pindex;
    }
    //5. 检查块的索引状态
    CheckBlockIndex(chainparams.GetConsensus());

    return true;
}

// Exposed wrapper for AcceptBlockHeader
bool ProcessNewBlockHeaders(const Config &config,
                            const std::vector<CBlockHeader> &headers,
                            CValidationState &state,
                            const CBlockIndex **ppindex) {
    {
        LOCK(cs_main);
        for (const CBlockHeader &header : headers) {
            // Use a temp pindex instead of ppindex to avoid a const_cast
            CBlockIndex *pindex = nullptr;
            if (!AcceptBlockHeader(config, header, state, &pindex)) {
                return false;
            }
            if (ppindex) {
                *ppindex = pindex;
            }
        }
    }
    NotifyHeaderTip();
    return true;
}

/**
 * Store block on disk. If dbp is non-null, the file is known to already reside
 * on disk.
 * 将区块数据存储在磁盘上。 如果dbp非空，这个文件是已经在磁盘上的文件
 * pblock(in) 将写入磁盘的块，state(out):写入时的状态，pindex(out): 创建该区块的索引，
 * fRequested(in):是否要求强制处理该区块；当块不是来自网络，或来自白名单节点的块数据
 * dbp(in): 块写入磁盘的位置；存在时，将要写入文件已知
 * fNewBlock(out):该区块是否为新块
 */
static bool AcceptBlock(const Config &config,
                        const std::shared_ptr<const CBlock> &pblock,
                        CValidationState &state, CBlockIndex **ppindex,
                        bool fRequested, const CDiskBlockPos *dbp,
                        bool *fNewBlock) {
    AssertLockHeld(cs_main);

    const CBlock &block = *pblock;
    if (fNewBlock) {
        *fNewBlock = false;
    }

    CBlockIndex *pindexDummy = nullptr;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;
    // pindex(out)  block(in)
    if (!AcceptBlockHeader(config, block, state, &pindex)) {
        return false;
    }

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    // 处理所有 requested = TRUE 的区块(此区块为新接收的块)；当 requested = false 时，仅处理
    // 块的链工作量 大于当前激活链最高块，且时间不是太超前的区块。
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreWork =
        (chainActive.Tip() ? pindex->nChainWork > chainActive.Tip()->nChainWork
                           : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    // 乱序的块限制了裁剪的效率，因为裁剪不能删除包含距离激活链Tip 近的区块文件。无论是否裁剪，
    // 都进行该测试，当不处理requested = false的块，一般是安全的。
    bool fTooFarAhead =
        (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: Decouple this function from the block download logic by removing
    // fRequested
    // This requires some new chain datastructure to efficiently look up if a
    // block is in a chain leading to a candidate for best tip, despite not
    // being such a candidate itself.

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.  这个块已存在，返回TRUE
    if (fAlreadyHave) {
        return true;
    }

    // If we didn't ask for it: 如果没有要求
    if (!fRequested) {
        // This is a previously-processed block that was pruned. 是一个已被处理但被裁减的区块。
        if (pindex->nTx != 0) {
            return true;
        }

        // Don't process less-work chains.没有足够的工作量，都不处理
        if (!fHasMoreWork) {
            return true;
        }

        // Block height is too high. 时间太靠前，都不处理
        if (fTooFarAhead) {
            return true;
        }
    }

    //是新块
    if (fNewBlock) {
        *fNewBlock = true;
    }

    const CChainParams &chainparams = config.GetChainParams();
    if (!CheckBlock(config, block, state, chainparams.GetConsensus()) ||
        !ContextualCheckBlock(config, block, state, chainparams.GetConsensus(),
                              pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s (block %s)", __func__, FormatStateMessage(state),
                     block.GetHash().ToString());
    }

    // Header is valid/has work, merkle tree and segwit merkle tree are
    // good...RELAY NOW (but if it does not build on our best tip, let the
    // SendMessages loop relay it)
    // 上述区块检查通过，为有效区块，开始向外中继；既非初始化下载，且Tip为该区块索引的父区块，标识接收到一个新Tip，开始向外广播。
    if (!IsInitialBlockDownload() && chainActive.Tip() == pindex->pprev) {
        GetMainSignals().NewPoWValidBlock(pindex, pblock);
    }

    int nHeight = pindex->nHeight;

    // Write block to history file
    try {
        //1. 获取块的序列化后的大小
        unsigned int nBlockSize =
            ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != nullptr) {
            blockPos = *dbp;
        }
        //2. 查找块文件，此处会对blockPos 进行赋值
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight,
                          block.GetBlockTime(), dbp != nullptr)) {
            return error("AcceptBlock(): FindBlockPos failed");
        }
        //3. 将块和交易数据写文件；将block和报头信息写入文件。(报头信息用来表示：当前的块是属于哪条链：主链，测试链，regtest)
        if (dbp == nullptr) {
            if (!WriteBlockToDisk(block, blockPos,
                                  chainparams.MessageStart())) {
                AbortNode(state, "Failed to write block");
            }
        }
        //4. 修改块的索引状态；标识已接受到一个块的所有交易信息
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos)) {
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
        }
    } catch (const std::runtime_error &e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }
    //5. 刷新数据到磁盘
    if (fCheckForPruning) {
        // we just allocated more disk space for block files.
        FlushStateToDisk(state, FLUSH_STATE_NONE);
    }

    return true;
}

bool ProcessNewBlock(const Config &config,
                     const std::shared_ptr<const CBlock> pblock,
                     bool fForceProcessing, bool *fNewBlock) {
    {
        CBlockIndex *pindex = nullptr;
        if (fNewBlock) *fNewBlock = false;

        const CChainParams &chainparams = config.GetChainParams();

        CValidationState state;
        // Ensure that CheckBlock() passes before calling AcceptBlock, as
        // belt-and-suspenders.
        bool ret = CheckBlock(config, *pblock, state, chainparams.GetConsensus());

        LOCK(cs_main);

        if (ret) {
            // Store to disk
            ret = AcceptBlock(config, pblock, state, &pindex, fForceProcessing,
                              nullptr, fNewBlock);
        }
        CheckBlockIndex(chainparams.GetConsensus());
        if (!ret) {
            GetMainSignals().BlockChecked(*pblock, state);
            return error("%s: AcceptBlock FAILED", __func__);
        }
    }

    NotifyHeaderTip();

    // Only used to report errors, not invalidity - ignore it
    CValidationState state;
    // pblock： 刚接收的区块
    if (!ActivateBestChain(config, state, pblock))
        return error("%s: ActivateBestChain failed", __func__);

    return true;
}

bool TestBlockValidity(const Config &config, CValidationState &state,
                       const CChainParams &chainparams, const CBlock &block,
                       CBlockIndex *pindexPrev, bool fCheckPOW,
                       bool fCheckMerkleRoot) {
    AssertLockHeld(cs_main);
    assert(pindexPrev && pindexPrev == chainActive.Tip());
    if (fCheckpointsEnabled &&
        !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams,
                                     block.GetHash())) {
        return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__,
                     state.GetRejectReason().c_str());
    }

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, chainparams.GetConsensus(),
                                    pindexPrev, GetAdjustedTime())) {
        return error("%s: Consensus::ContextualCheckBlockHeader: %s", __func__,
                     FormatStateMessage(state));
    }
    if (!CheckBlock(config, block, state, chainparams.GetConsensus(), fCheckPOW,
                    fCheckMerkleRoot)) {
        return error("%s: Consensus::CheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }
    if (!ContextualCheckBlock(config, block, state, chainparams.GetConsensus(),
                              pindexPrev)) {
        return error("%s: Consensus::ContextualCheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }
    if (!ConnectBlock(config, block, state, &indexDummy, viewNew, chainparams,
                      true)) {
        return false;
    }

    assert(state.IsValid());
    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage() {
    uint64_t retval = 0;
    for (const CBlockFileInfo &file : vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

/* Prune a block file (modify associated database entries)
 * 修剪一个块文件;修改该文件中所有块索引的状态，并将这个文件号加入 setDirtyFileInfo 全局状态
 * 设置该文件号对应的 全局状态文件信息为NULL；vinfoBlockFile[number];
 * 然后将该文件中包含的所有的块索引加入 setDirtyBlockIndex全局状态中，标识这些块的数据将被裁减
 * */
void PruneOneBlockFile(const int fileNumber) {
    //先查全局状态的所有块索引
    for (BlockMap::iterator it = mapBlockIndex.begin();
         it != mapBlockIndex.end(); ++it) {
        CBlockIndex *pindex = it->second;
        //如果该块数据 存在于这个要删除的文件中
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;        //修改块的索引状态(因为裁剪后，本地就不再含有该块的交易数据)
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;        //理由同上
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            // 我们所有修剪的区块，都需要放入mapBlocksUnlinked中，以便可以再次下载来验证该区块在链上，
            // 在这个点
            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                range = mapBlocksUnlinked.equal_range(pindex->pprev);
            // 查找全局状态 孤儿块的集合中 是否含有该块的父区块。
            // 如果存在，
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator _it =
                    range.first;        // 指向第一个pair对象
                range.first++;
                // 如果该迭代器指向的 子区块等于要删除的区块，就在全局变量中删除该迭代器。
                if (_it->second == pindex) {
                    mapBlocksUnlinked.erase(_it);
                }
            }
        }
    }

    //设置要删除的文件信息为NULL，并将该文件号 加入全局状态中
    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}

void UnlinkPrunedFiles(const std::set<int> &setFilesToPrune) {
    for (std::set<int>::iterator it = setFilesToPrune.begin();
         it != setFilesToPrune.end(); ++it) {
        CDiskBlockPos pos(*it, 0);
        boost::filesystem::remove(GetBlockPosFilename(pos, "blk"));
        boost::filesystem::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

/**
 * Calculate the block/rev files to delete based on height specified by user
 * with RPC command pruneblockchain.
 * 依据用户指定的高度，计算将要删除的文件，并删除这些文件
 * setFilesToPrune(out):写入已被删除的文件号；nManualPruneHeight(in):将要删除该高度下的所有文件
 */
static void FindFilesToPruneManual(std::set<int> &setFilesToPrune,
                                   int nManualPruneHeight) {
    //此时必须处于修剪模式下，且修剪的高度必须大于0
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr) {
        return;
    }

    // last block to prune is the lesser of (user-specified height,
    // MIN_BLOCKS_TO_KEEP from the tip)
    // 获取可以裁剪的最大高度；即该高度之下的块文件都可以裁剪
    unsigned int nLastBlockWeCanPrune =
        std::min((unsigned)nManualPruneHeight,
                 chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP);
    int count = 0;
    for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
        //查看当前的文件是否处于可修剪的状态
        if (vinfoBlockFile[fileNumber].nSize == 0 ||
            vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
            continue;
        }
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    //打印日志: count：已修改的文件个数；nManualPruneHeight:最高的修剪高度。
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n",
              nLastBlockWeCanPrune, count);
}

/* This function is called from the RPC code for pruneblockchain */
void PruneBlockFilesManual(int nManualPruneHeight) {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_NONE, nManualPruneHeight);
}

/* Calculate the block/rev files that should be deleted to remain under target*/
void FindFilesToPrune(std::set<int> &setFilesToPrune,
                      uint64_t nPruneAfterHeight) {
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr || nPruneTarget == 0) {
        return;
    }
    if (uint64_t(chainActive.Tip()->nHeight) <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune =
        chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files,
    // so we should leave a buffer under our target to account for another
    // allocation before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize +
                            vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0) {
                continue;
            }

            // are we below our target?
            if (nCurrentUsage + nBuffer < nPruneTarget) {
                break;
            }

            // don't prune files that could have a block within
            // MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
                continue;
            }

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint("prune", "Prune: target=%dMiB actual=%dMiB diff=%dMiB "
                      "max_prune_height=%d removed %d blk/rev pairs\n",
             nPruneTarget / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             ((int64_t)nPruneTarget - (int64_t)nCurrentUsage) / 1024 / 1024,
             nLastBlockWeCanPrune, count);
}

//检测磁盘的空间是否足够；
bool CheckDiskSpace(uint64_t nAdditionalBytes) {
    //获取当前磁盘可用的空间
    uint64_t nFreeBytesAvailable =
        boost::filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)；检测空间是否够用
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

//打开文件，并将它偏移一定的位置，然后返回文件描述符。
FILE *OpenDiskFile(const CDiskBlockPos &pos, const char *prefix,
                   bool fReadOnly) {
    if (pos.IsNull()) return nullptr;
    // 获取文件路径
    boost::filesystem::path path = GetBlockPosFilename(pos, prefix);
    // 创建目录，打开文件
    boost::filesystem::create_directories(path.parent_path());
    FILE *file = fopen(path.string().c_str(), "rb+");
    //如果文件不存在，并且权限非只读的，则创建一个文件
    if (!file && !fReadOnly) file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return nullptr;
    }
    if (pos.nPos) {
        //移动文件指针的偏移位置，
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos,
                      path.string());
            fclose(file);
            return nullptr;
        }
    }
    return file;
}

FILE *OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE *OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path GetBlockPosFilename(const CDiskBlockPos &pos,
                                            const char *prefix) {
    return GetDataDir() / "blocks" /    strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex *InsertBlockIndex(uint256 hash) {
    if (hash.IsNull()) return nullptr;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end()) return (*mi).second;

    // Create new
    CBlockIndex *pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw std::runtime_error(std::string(__func__) +
                                 ": new CBlockIndex failed");
    mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

static bool LoadBlockIndexDB(const CChainParams &chainparams) {
    if (!pblocktree->LoadBlockIndexGuts(InsertBlockIndex)) return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex *>> vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<uint256, CBlockIndex *> &item : mapBlockIndex) {
        CBlockIndex *pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex *> &item : vSortedByHeight) {
        CBlockIndex *pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) +
                             GetBlockProof(*pindex);
        pindex->nTimeMax =
            (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime)
                           : pindex->nTime);
        // We can link the chain of blocks for which we've received transactions
        // at some point. Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(
                        std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) &&
            (pindex->nChainTx || pindex->pprev == nullptr)) {
            setBlockIndexCandidates.insert(pindex);
        }
        if (pindex->nStatus & BLOCK_FAILED_MASK &&
            (!pindexBestInvalid ||
             pindex->nChainWork > pindexBestInvalid->nChainWork)) {
            pindexBestInvalid = pindex;
        }
        if (pindex->pprev) {
            pindex->BuildSkip();
        }
        if (pindex->IsValid(BLOCK_VALID_TREE) &&
            (pindexBestHeader == nullptr ||
             CBlockIndexWorkComparator()(pindexBestHeader, pindex))) {
            pindexBestHeader = pindex;
        }
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__,
              vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<uint256, CBlockIndex *> &item : mapBlockIndex) {
        CBlockIndex *pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }

    //这一步是去判断这些文件是否存在，不存在直接返回false. 如果存在，就继续向下运行
    for (std::set<int>::iterator it = setBlkDataFiles.begin();
         it != setBlkDataFiles.end(); it++) {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION)
                .IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned) {
        LogPrintf(
            "LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("%s: transaction index %s\n", __func__,
              fTxIndex ? "enabled" : "disabled");

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end()) {
        return true;
    }
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    LogPrintf(
        "%s: hashBestChain=%s height=%d date=%s progress=%f\n", __func__,
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
                          chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainparams.TxData(), chainActive.Tip()));

    return true;
}

CVerifyDB::CVerifyDB() {
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB() {
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(const Config &config, const CChainParams &chainparams,
                         CCoinsView *coinsview, int nCheckLevel,
                         int nCheckDepth) {
    LOCK(cs_main);
    if (chainActive.Tip() == nullptr || chainActive.Tip()->pprev == nullptr) {
        return true;
    }

    // Verify blocks in the best chain
    if (nCheckDepth <= 0) {
        // suffices until the year 19000
        nCheckDepth = 1000000000;
    }
    if (nCheckDepth > chainActive.Height()) {
        nCheckDepth = chainActive.Height();
    }
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth,
              nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex *pindexState = chainActive.Tip();
    CBlockIndex *pindexFailure = nullptr;
    int nGoodTransactions = 0;
    CValidationState state;
    int reportDone = 0;
    LogPrintf("[0%%]...");
    for (CBlockIndex *pindex = chainActive.Tip(); pindex && pindex->pprev;
         pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(
            1, std::min(
                   99,
                   (int)(((double)(chainActive.Height() - pindex->nHeight)) /
                         (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));

        if (reportDone < percentageDone / 10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone);
            reportDone = percentageDone / 10;
        }

        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone);
        if (pindex->nHeight < chainActive.Height() - nCheckDepth) {
            break;
        }

        if (fPruneMode && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, only go back as far as we have data.
            LogPrintf("VerifyDB(): block verification stopping at height %d "
                      "(pruning, no data)\n",
                      pindex->nHeight);
            break;
        }
        CBlock block;

        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus())) {
            return error(
                "VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s",
                pindex->nHeight, pindex->GetBlockHash().ToString());
        }

        // check level 1: verify block validity
        if (nCheckLevel >= 1 &&
            !CheckBlock(config, block, state, chainparams.GetConsensus())) {
            return error("%s: *** found bad block at %d, hash=%s (%s)\n",
                         __func__, pindex->nHeight,
                         pindex->GetBlockHash().ToString(),
                         FormatStateMessage(state));
        }

        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos,
                                      pindex->pprev->GetBlockHash())) {
                    return error(
                        "VerifyDB(): *** found bad undo data at %d, hash=%s\n",
                        pindex->nHeight, pindex->GetBlockHash().ToString());
                }
            }
        }

        // check level 3: check for inconsistencies during memory-only
        // disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState &&
            (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <=
                nCoinCacheUsage) {
            DisconnectResult res = DisconnectBlock(block, pindex, coins);
            if (res == DISCONNECT_FAILED) {
                return error("VerifyDB(): *** irrecoverable inconsistency in "
                             "block data at %d, hash=%s",
                             pindex->nHeight,
                             pindex->GetBlockHash().ToString());
            }
            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }

        if (ShutdownRequested()) {
            return true;
        }
    }

    if (pindexFailure) {
        return error("VerifyDB(): *** coin database inconsistencies found "
                     "(last %i blocks, %i good transactions before that)\n",
                     chainActive.Height() - pindexFailure->nHeight + 1,
                     nGoodTransactions);
    }

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(
                _("Verifying blocks..."),
                std::max(
                    1, std::min(99, 100 - (int)(((double)(chainActive.Height() -
                                                          pindex->nHeight)) /
                                                (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus())) {
                return error(
                    "VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s",
                    pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            if (!ConnectBlock(config, block, state, pindex, coins,
                              chainparams)) {
                return error(
                    "VerifyDB(): *** found unconnectable block at %d, hash=%s",
                    pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
    }

    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i "
              "transactions)\n",
              chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

//当在激活链上的块有丢失数据时，倒推链的状态，并将它从块的索引中移除。
bool RewindBlockIndex(const Config &config, const CChainParams &params) {
    LOCK(cs_main);

    // 当前激活链将要出块的高度
    int nHeight = chainActive.Height() + 1;

    // nHeight is now the height of the first insufficiently-validated block, or
    // tipheight + 1；
    CValidationState state;
    CBlockIndex *pindex = chainActive.Tip();
    //
    while (chainActive.Height() >= nHeight) {
        // 处于裁剪模式，且激活链的Tip块没有块的完整数据，退出循环
        if (fPruneMode && !(chainActive.Tip()->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, don't try rewinding past the HAVE_DATA point; since
            // older blocks can't be served anyway, there's no need to walk
            // further, and trying to DisconnectTip() will fail (and require a
            // needless reindex/redownload of the blockchain).
            break;
        }
        // 拒绝Tip
        if (!DisconnectTip(config, state, true)) {
            return error(
                "RewindBlockIndex: unable to disconnect block at height %i",
                pindex->nHeight);
        }
        // Occasionally flush state to disk.  刷新状态到磁盘
        if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
            return false;
        }
    }

    // Reduce validity flag and have-data flags.  降低有效性的flag和存在数据的flag。
    // We do this after actual disconnecting, otherwise we'll end up writing the
    // lack of data to disk before writing the chainstate, resulting in a
    // failure to continue if interrupted.
    // 在拒绝完块之后再做下面的操作，否则在写入链状态之前进行操作会缺少将要写入磁盘的数据，会导致在终端时无法继续。
    // 遍历全局状态的所有块索引，将有效性的块放入 全局的候选Tip map内。
    for (BlockMap::iterator it = mapBlockIndex.begin();
         it != mapBlockIndex.end(); it++) {
        CBlockIndex *pindexIter = it->second;

        if (pindexIter->IsValid(BLOCK_VALID_TRANSACTIONS) &&
            pindexIter->nChainTx) {
            setBlockIndexCandidates.insert(pindexIter);
        }
    }
    // 裁剪块索引
    PruneBlockIndexCandidates();
    // 检查索引
    CheckBlockIndex(params.GetConsensus());
    // 写数据到磁盘
    if (!FlushStateToDisk(state, FLUSH_STATE_ALWAYS)) {
        return false;
    }

    return true;
}

// May NOT be used after any connections are up as much of the peer-processing
// logic assumes a consistent block index state
void UnloadBlockIndex() {
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(nullptr);
    pindexBestInvalid = nullptr;
    pindexBestHeader = nullptr;
    mempool.clear();
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    versionbitscache.Clear();
    for (int b = 0; b < VERSIONBITS_NUM_BITS; b++) {
        warningcache[b].clear();
    }

    for (BlockMap::value_type &entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
    fHavePruned = false;
}

bool LoadBlockIndex(const CChainParams &chainparams) {
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB(chainparams)) {
        return false;
    }
    return true;
}

bool InitBlockIndex(const Config &config) {
    LOCK(cs_main);

    // Check whether we're already initialized
    if (chainActive.Genesis() != nullptr) {
        return true;
    }

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", DEFAULT_TXINDEX);
    pblocktree->WriteFlag("txindex", fTxIndex);
    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the
    // one already on disk)
    if (!fReindex) {
        try {
            const CChainParams &chainparams = config.GetChainParams();
            CBlock &block = const_cast<CBlock &>(chainparams.GenesisBlock());
            // Start new block file
            unsigned int nBlockSize =
                ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0,
                              block.GetBlockTime())) {
                return error("LoadBlockIndex(): FindBlockPos failed");
            }
            if (!WriteBlockToDisk(block, blockPos,
                                  chainparams.MessageStart())) {
                return error(
                    "LoadBlockIndex(): writing genesis block to disk failed");
            }
            CBlockIndex *pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos)) {
                return error("LoadBlockIndex(): genesis block not accepted");
            }
            // Force a chainstate write so that when we VerifyDB in a moment, it
            // doesn't check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error &e) {
            return error(
                "LoadBlockIndex(): failed to initialize block database: %s",
                e.what());
        }
    }

    return true;
}

bool LoadExternalBlockFile(const Config &config, FILE *fileIn,
                           CDiskBlockPos *dbp) {
    // Map of disk positions for blocks with unknown parent (only used for
    // reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    const CChainParams &chainparams = config.GetChainParams();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile
        // destructor. Make sure we have at least 2*MAX_TX_SIZE space in there
        // so any transaction can fit in the buffer.
        // 缓冲区有2M的数据
        CBufferedFile blkdat(fileIn, 2 * MAX_TX_SIZE, MAX_TX_SIZE + 8, SER_DISK,
                             CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            // Start one byte further next time, in case of failure.
            nRewind++;
            // Remove former limit.
            blkdat.SetLimit();
            unsigned int nSize = 0;
            try {
                // Locate a header.
                uint8_t buf[CMessageHeader::MESSAGE_START_SIZE];
                //
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(),
                           CMessageHeader::MESSAGE_START_SIZE)) {
                    continue;
                }
                // Read size.
                blkdat >> nSize;
                if (nSize < 80) {
                    continue;
                }
            } catch (const std::exception &) {
                // No valid block header found; don't complain.
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp) {
                    dbp->nPos = nBlockPos;
                }
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
                CBlock &block = *pblock;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock &&
                    mapBlockIndex.find(block.hashPrevBlock) ==
                        mapBlockIndex.end()) {
                    LogPrint("reindex",
                             "%s: Out of order block %s, parent %s not known\n",
                             __func__, hash.ToString(),
                             block.hashPrevBlock.ToString());
                    if (dbp) {
                        mapBlocksUnknownParent.insert(
                            std::make_pair(block.hashPrevBlock, *dbp));
                    }
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 ||
                    (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    LOCK(cs_main);
                    CValidationState state;
                    if (AcceptBlock(config, pblock, state, nullptr, true, dbp,
                                    nullptr)) {
                        nLoaded++;
                    }
                    if (state.IsError()) {
                        break;
                    }
                } else if (hash !=
                               chainparams.GetConsensus().hashGenesisBlock &&
                           mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrint(
                        "reindex",
                        "Block Import: already had block %s at height %d\n",
                        hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Activate the genesis block so normal node progress can
                // continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(config, state)) {
                        break;
                    }
                }

                NotifyHeaderTip();

                // Recursively process earlier encountered successors of this
                // block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator,
                              std::multimap<uint256, CDiskBlockPos>::iterator>
                        range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it =
                            range.first;
                        std::shared_ptr<CBlock> pblockrecursive =
                            std::make_shared<CBlock>();
                        if (ReadBlockFromDisk(*pblockrecursive, it->second,
                                              chainparams.GetConsensus())) {
                            LogPrint(
                                "reindex",
                                "%s: Processing out of order child %s of %s\n",
                                __func__, pblockrecursive->GetHash().ToString(),
                                head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (AcceptBlock(config, pblockrecursive, dummy,
                                            nullptr, true, &it->second,
                                            nullptr)) {
                                nLoaded++;
                                queue.push_back(pblockrecursive->GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception &e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__,
                          e.what());
            }
        }
    } catch (const std::runtime_error &e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0) {
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded,
                  GetTimeMillis() - nStart);
    }
    return nLoaded > 0;
}

//检查全局状态中块的索引；每当接收到一个块时，都会先检查块，符合条件，创建该块的索引，并更新其索引的状态，
// 然后加入全局状态(mapBlockIndex), 然后调用该函数，对全局状态中的包含的所有块索引进行检查
static void CheckBlockIndex(const Consensus::Params &consensusParams) {
    //1. 不检查索引
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex
    // before ActivateBestChain, so we have the genesis block in mapBlockIndex
    // but no active chain. (A few of the tests when iterating the block tree
    // require that chainActive has been initialized.)
    //在reindex期间，从创世块读取，并且在ActivateBestChain之前调用 CheckBlockIndex，所以在没有激活链时，
    // 在全局状态中就含有创世块。
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree. 构建一个向前指向的完整区块树。
    // 存储的内容为 ： pprevIndex --> pindex; 即键为父区块索引，值为当前块索引
    std::multimap<CBlockIndex *, CBlockIndex *> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin();
         it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
              std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
        rangeGenesis = forward.equal_range(nullptr);  //查找键值为 nil的区块，即创世区块

    CBlockIndex *pindex = rangeGenesis.first->second;  //此时执行创世区块
    rangeGenesis.first++;       //将索引向后移动
    // There is only one index entry with parent nullptr. 创世区块只在全局状态中， 含有一个条目
    assert(rangeGenesis.first == rangeGenesis.second);

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    // Oldest ancestor of pindex which is invalid.
    CBlockIndex *pindexFirstInvalid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex *pindexFirstMissing = nullptr;
    // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex *pindexFirstNeverProcessed = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotTreeValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotTransactionsValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotChainValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotScriptsValid = nullptr;
    //此时pindex 刚进入下面循环时，指向创世区块
    while (pindex != nullptr) {
        nNodes++;
        if (pindexFirstInvalid == nullptr &&
            pindex->nStatus & BLOCK_FAILED_VALID) {
            pindexFirstInvalid = pindex;
        }
        if (pindexFirstMissing == nullptr &&
            !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            pindexFirstMissing = pindex;
        }
        if (pindexFirstNeverProcessed == nullptr && pindex->nTx == 0) {
            pindexFirstNeverProcessed = pindex;
        }
        //当pindex指向为非 创世区块时
        if (pindex->pprev != nullptr && pindexFirstNotTreeValid == nullptr &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) {
            pindexFirstNotTreeValid = pindex;
        }
        if (pindex->pprev != nullptr &&
            pindexFirstNotTransactionsValid == nullptr &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) {
            pindexFirstNotTransactionsValid = pindex;
        }
        if (pindex->pprev != nullptr && pindexFirstNotChainValid == nullptr &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) {
            pindexFirstNotChainValid = pindex;
        }
        if (pindex->pprev != nullptr && pindexFirstNotScriptsValid == nullptr &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) {
            pindexFirstNotScriptsValid = pindex;
        }

        // Begin: actual consistency checks.
        if (pindex->pprev == nullptr) {
            // 创世块
            // Genesis block checks.
            // Genesis block's hash must match.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock);
            // The current active chain's genesis block must be this block.
            assert(pindex == chainActive.Genesis());
        }
        if (pindex->nChainTx == 0) {
            // nSequenceId can't be set positive for blocks that aren't linked
            // (negative is used for preciousblock)
            assert(pindex->nSequenceId <= 0);
        }
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or
        // not pruning has occurred). HAVE_DATA is only equivalent to nTx > 0
        // (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx
            // > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies
            // nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) {
            assert(pindex->nStatus & BLOCK_HAVE_DATA);
        }
        // This is pruning-independent., 当单独裁剪时，pindex的status 应当 大于 nTx 的状态相同
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >=
                BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0));
        // All parents having had data (at some point) is equivalent to all
        // parents being VALID_TRANSACTIONS, which is equivalent to nChainTx
        // being set.
        // nChainTx != 0 is used to signal that all parent blocks have been
        // processed (but may have been pruned).
        // 所有的付款有数据 标识 所有的父块的状态都为VALID_TRANSACTIONS，此时  nChainTx 将被设置。
        // 当nChainTx被设置时，标识所有的父块已被处理；(但是这些父块也可能已被裁剪)
        assert((pindexFirstNeverProcessed != nullptr) ==
               (pindex->nChainTx == 0));
        assert((pindexFirstNotTransactionsValid != nullptr) ==
               (pindex->nChainTx == 0));
        // nHeight must be consistent.
        assert(pindex->nHeight == nHeight);
        // For every block except the genesis block, the chainwork must be
        // larger than the parent's. 对于除了创世块的所有块，它的链工作量都必须大于它的父区块
        assert(pindex->pprev == nullptr ||
               pindex->nChainWork >= pindex->pprev->nChainWork);
        // The pskip pointer must point back for all but the first 2 blocks.
        assert(nHeight < 2 ||
               (pindex->pskip && (pindex->pskip->nHeight < nHeight)));
        // All mapBlockIndex entries must at least be TREE valid
        assert(pindexFirstNotTreeValid == nullptr);
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) {
            // TREE valid implies all parents are TREE valid
            assert(pindexFirstNotTreeValid == nullptr);
        }
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) {
            // CHAIN valid implies all parents are CHAIN valid
            assert(pindexFirstNotChainValid == nullptr);
        }
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) {
            // SCRIPTS valid implies all parents are SCRIPTS valid
            assert(pindexFirstNotScriptsValid == nullptr);
        }
        if (pindexFirstInvalid == nullptr) {
            // Checks for not-invalid blocks.
            // The failed mask cannot be set for blocks without invalid parents.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0);
        }
        // pindex的链工作量比激活链的工作量小，或接收顺序小于激活链的顶端，或直接指针顺序小于激活链的最顶端，进入下面条件
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) &&
            pindexFirstNeverProcessed == nullptr) {
            if (pindexFirstInvalid == nullptr) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates. chainActive.Tip() must also be there
                // even if some data has been pruned.
                if (pindexFirstMissing == nullptr ||
                    pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block
                // was in setBlockIndexCandidates but had to be removed because
                // of the missing data. In this case it must be in
                // mapBlocksUnlinked -- see test below.
            }
        } else {
            // If this block sorts worse than the current tip or some ancestor's
            // block has never been seen, it cannot be in
            // setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                  std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
            rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) &&
            pindexFirstNeverProcessed != nullptr &&
            pindexFirstInvalid == nullptr) {
            // If this block has block data available, some parent was never
            // received, and has no invalid parents, it must be in
            // mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
            assert(!foundInUnlinked);
        }
        if (pindexFirstMissing == nullptr) {
            // We aren't missing data for any parent -- cannot be in
            // mapBlocksUnlinked. 任何没有丢失祖先区块的数据不可以在 mapBlocksUnlinked
            assert(!foundInUnlinked);
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) &&
            pindexFirstNeverProcessed == nullptr &&
            pindexFirstMissing != nullptr) {
            // We HAVE_DATA for this block, have received data for all parents
            // at some point, but we're currently missing data for some parent.
            // We must have pruned.
            assert(fHavePruned);
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it
            // wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) &&
                setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == nullptr) {
                    assert(foundInUnlinked);
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash());
        // // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.; 查找创世交易的后代交易
        std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                  std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
            range = forward.equal_range(pindex);
        //当在全局状态中查找到 以该交易作为父区块的 后代区块存在时，将pindex 指向接下来的后代区块，并将高度向后移动，然后=进行下一轮循环
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node. Move upwards until we reach a node of which we
        // have not yet visited the last child. 继续向前移动，直到找到一个孩子节点
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the
            // corresponding variable.
            // 要么移动到pindex的父区块，要么移动到pindex的兄弟区块。如果pindex是第一个具有某种属性的对象，则不要设置对应的变量值
            if (pindex == pindexFirstInvalid) {
                pindexFirstInvalid = nullptr;
            }
            if (pindex == pindexFirstMissing) {
                pindexFirstMissing = nullptr;
            }
            if (pindex == pindexFirstNeverProcessed) {
                pindexFirstNeverProcessed = nullptr;
            }
            if (pindex == pindexFirstNotTreeValid) {
                pindexFirstNotTreeValid = nullptr;
            }
            if (pindex == pindexFirstNotTransactionsValid) {
                pindexFirstNotTransactionsValid = nullptr;
            }
            if (pindex == pindexFirstNotChainValid) {
                pindexFirstNotChainValid = nullptr;
            }
            if (pindex == pindexFirstNotScriptsValid) {
                pindexFirstNotScriptsValid = nullptr;
            }
            // Find our parent.
            CBlockIndex *pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                // Our parent must have at least the node we're coming from as
                // child.
                assert(rangePar.first != rangePar.second);
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

std::string CBlockFileInfo::ToString() const {
    return strprintf(
        "CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)",
        nBlocks, nSize, nHeightFirst, nHeightLast,
        DateTimeStrFormat("%Y-%m-%d", nTimeFirst),
        DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}

CBlockFileInfo *GetBlockFileInfo(size_t n) {
    return &vinfoBlockFile.at(n);
}

ThresholdState VersionBitsTipState(const Consensus::Params &params,
                                   Consensus::DeploymentPos pos) {
    LOCK(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

int VersionBitsTipStateSinceHeight(const Consensus::Params &params,
                                   Consensus::DeploymentPos pos) {
    LOCK(cs_main);
    return VersionBitsStateSinceHeight(chainActive.Tip(), params, pos,
                                       versionbitscache);
}

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

bool LoadMempool(const Config &config) {
    int64_t nExpiryTimeout =
        GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60;
    FILE *filestr =
        fopen((GetDataDir() / "mempool.dat").string().c_str(), "rb");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf(
            "Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t skipped = 0;
    int64_t failed = 0;
    int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }
        uint64_t num;
        file >> num;
        double prioritydummy = 0;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            Amount amountdelta = nFeeDelta;
            if (amountdelta != 0) {
                mempool.PrioritiseTransaction(tx->GetId(),
                                              tx->GetId().ToString(),
                                              prioritydummy, amountdelta);
            }
            CValidationState state;
            if (nTime + nExpiryTimeout > nNow) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(config, mempool, state, tx, true,
                                           nullptr, nTime);
                if (state.IsValid()) {
                    ++count;
                } else {
                    ++failed;
                }
            } else {
                ++skipped;
            }
            if (ShutdownRequested()) return false;
        }
        std::map<uint256, Amount> mapDeltas;
        file >> mapDeltas;

        for (const auto &i : mapDeltas) {
            mempool.PrioritiseTransaction(i.first, i.first.ToString(),
                                          prioritydummy, i.second);
        }
    } catch (const std::exception &e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing "
                  "anyway.\n",
                  e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i successes, %i "
              "failed, %i expired\n",
              count, failed, skipped);
    return true;
}

//将mempool中的 所有条目写入文件。
void DumpMempool(void) {
    int64_t start = GetTimeMicros();

    std::map<uint256, Amount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    {
        LOCK(mempool.cs);
        for (const auto &i : mempool.mapDeltas) {
            mapDeltas[i.first] = i.second.second.GetSatoshis();
        }
        vinfo = mempool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE *filestr =
            fopen((GetDataDir() / "mempool.dat.new").string().c_str(), "wb");
        if (!filestr) {
            return;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)vinfo.size();
        for (const auto &i : vinfo) {
            file << *(i.tx);
            file << (int64_t)i.nTime;
            file << (int64_t)i.nFeeDelta.GetSatoshis();
            mapDeltas.erase(i.tx->GetId());
        }

        file << mapDeltas;
        FileCommit(file.Get());
        file.fclose();
        RenameOver(GetDataDir() / "mempool.dat.new",
                   GetDataDir() / "mempool.dat");
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n",
                  (mid - start) * 0.000001, (last - mid) * 0.000001);
    } catch (const std::exception &e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
    }
}

//! Guess how far we are in the verification process at the given block index
double GuessVerificationProgress(const ChainTxData &data, CBlockIndex *pindex) {
    // 给出的验证索引为null, 所以验证进程为0.0
    if (pindex == nullptr) return 0.0;

    // 获取当前的时间
    int64_t nNow = time(nullptr);

    double fTxTotal;

    // 如果当前给定的 块索引中的链上交易小于 网络中接收的链上的交易；当前的交易数量+交易的速率*时间 = 这段时间的总交易数量
    if (pindex->nChainTx <= data.nTxCount) {
        fTxTotal = data.nTxCount + (nNow - data.nTime) * data.dTxRate;
    } else {
        fTxTotal =
            pindex->nChainTx + (nNow - pindex->GetBlockTime()) * data.dTxRate;
    }
    // 计算当前给定的块索引的数量 与总交易量的比值，即可大致获取当前验证的进程
    return pindex->nChainTx / fTxTotal;
}

class CMainCleanup {
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
    }
} instance_of_cmaincleanup;
