//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/LedgerHistory.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/chrono.h>
#include <ripple/basics/contract.h>
#include <ripple/json/to_string.h>

namespace ripple {

// FIXME: Need to clean up ledgers by index at some point

LedgerHistory::LedgerHistory(
    beast::insight::Collector::ptr const& collector,
    Application& app)
    : app_(app)
    , collector_(collector)
    , mismatch_counter_(collector->make_counter("ledger.history", "mismatch"))
    , ledgerCache_(
          "LedgerCache",
          app_.config().getValueFor(SizedItem::ledgerSize),
          std::chrono::seconds{app_.config().getValueFor(SizedItem::ledgerAge)},
          stopwatch(),
          app_.journal("TaggedCache"))
    , consensusValidated_(
          "ConsensusValidated",
          64,
          std::chrono::minutes{5},
          stopwatch(),
          app_.journal("TaggedCache"))
    , j_(app.journal("LedgerHistory"))
{
}

bool
LedgerHistory::insert(std::shared_ptr<Ledger const> ledger, bool validated)
{
    if (!ledger->isImmutable())
        LogicError("mutable Ledger in insert");

    assert(ledger->stateMap().getHash().isNonZero());

    const bool alreadyHad =
        ledgerCache_.insert_or_assign(ledger->info().hash, ledger);

    std::unique_lock sl(mutex_);

    if (validated)
        mLedgersByIndex[ledger->info().seq] = ledger->info().hash;

    return alreadyHad;
}

LedgerHash
LedgerHistory::getLedgerHash(LedgerIndex index)
{
    std::unique_lock sl(mutex_);

    if (auto it = mLedgersByIndex.find(index); it != mLedgersByIndex.end())
        return it->second;

    return {};
}

std::shared_ptr<Ledger const>
LedgerHistory::getLedgerBySeq(LedgerIndex index)
{
    {
        std::unique_lock sl(mutex_);

        if (auto it = mLedgersByIndex.find(index); it != mLedgersByIndex.end())
        {
            uint256 hash = it->second;
            sl.unlock();
            return getLedgerByHash(hash);
        }
    }

    std::shared_ptr<Ledger const> ret = loadByIndex(index, app_);

    if (!ret)
        return ret;

    assert(ret->info().seq == index);
    assert(ret->isImmutable());
    ledgerCache_.retrieve_or_insert(ret->info().hash, ret);

    {
        // Add this ledger to the local tracking by index
        std::lock_guard sl(mutex_);
        mLedgersByIndex[ret->info().seq] = ret->info().hash;
        return (ret->info().seq == index) ? ret : nullptr;
    }
}

std::shared_ptr<Ledger const>
LedgerHistory::getLedgerByHash(LedgerHash const& hash)
{
    auto ret = ledgerCache_.fetch(hash);

    if (ret)
    {
        assert(ret->isImmutable());
        assert(ret->info().hash == hash);
        return ret;
    }

    ret = loadByHash(hash, app_);

    if (!ret)
        return ret;

    assert(ret->isImmutable());
    assert(ret->info().hash == hash);
    ledgerCache_.retrieve_or_insert(ret->info().hash, ret);
    assert(ret->info().hash == hash);

    return ret;
}

static void
log_one(
    ReadView const& ledger,
    uint256 const& tx,
    char const* msg,
    beast::Journal& j)
{
    auto metaData = ledger.txRead(tx).second;

    if (metaData != nullptr)
    {
        JLOG(j.debug()) << "MISMATCH on TX " << tx << ": " << msg
                        << " is missing this transaction:\n"
                        << metaData->getJson(JsonOptions::none);
    }
    else
    {
        JLOG(j.debug()) << "MISMATCH on TX " << tx << ": " << msg
                        << " is missing this transaction.";
    }
}

static void
log_metadata_difference(
    ReadView const& builtLedger,
    ReadView const& validLedger,
    uint256 const& tx,
    beast::Journal j)
{
    auto getMeta = [](ReadView const& ledger,
                      uint256 const& txID) -> std::shared_ptr<TxMeta> {
        auto meta = ledger.txRead(txID).second;
        if (!meta)
            return {};
        return std::make_shared<TxMeta>(txID, ledger.seq(), *meta);
    };

    auto validMetaData = getMeta(validLedger, tx);
    auto builtMetaData = getMeta(builtLedger, tx);
    assert(validMetaData != nullptr || builtMetaData != nullptr);

    if (validMetaData != nullptr && builtMetaData != nullptr)
    {
        auto const& validNodes = validMetaData->getNodes();
        auto const& builtNodes = builtMetaData->getNodes();

        bool const result_diff =
            validMetaData->getResultTER() != builtMetaData->getResultTER();

        bool const index_diff =
            validMetaData->getIndex() != builtMetaData->getIndex();

        bool const nodes_diff = validNodes != builtNodes;

        if (!result_diff && !index_diff && !nodes_diff)
        {
            JLOG(j.error()) << "MISMATCH on TX " << tx
                            << ": No apparent mismatches detected!";
            return;
        }

        if (!nodes_diff)
        {
            if (result_diff && index_diff)
            {
                JLOG(j.debug()) << "MISMATCH on TX " << tx
                                << ": Different result and index!";
                JLOG(j.debug()) << " Built:"
                                << " Result: " << builtMetaData->getResult()
                                << " Index: " << builtMetaData->getIndex();
                JLOG(j.debug()) << " Valid:"
                                << " Result: " << validMetaData->getResult()
                                << " Index: " << validMetaData->getIndex();
            }
            else if (result_diff)
            {
                JLOG(j.debug())
                    << "MISMATCH on TX " << tx << ": Different result!";
                JLOG(j.debug()) << " Built:"
                                << " Result: " << builtMetaData->getResult();
                JLOG(j.debug()) << " Valid:"
                                << " Result: " << validMetaData->getResult();
            }
            else if (index_diff)
            {
                JLOG(j.debug())
                    << "MISMATCH on TX " << tx << ": Different index!";
                JLOG(j.debug()) << " Built:"
                                << " Index: " << builtMetaData->getIndex();
                JLOG(j.debug()) << " Valid:"
                                << " Index: " << validMetaData->getIndex();
            }
        }
        else
        {
            if (result_diff && index_diff)
            {
                JLOG(j.debug()) << "MISMATCH on TX " << tx
                                << ": Different result, index and nodes!";
                JLOG(j.debug()) << " Built:\n"
                                << builtMetaData->getJson(JsonOptions::none);
                JLOG(j.debug()) << " Valid:\n"
                                << validMetaData->getJson(JsonOptions::none);
            }
            else if (result_diff)
            {
                JLOG(j.debug()) << "MISMATCH on TX " << tx
                                << ": Different result and nodes!";
                JLOG(j.debug())
                    << " Built:"
                    << " Result: " << builtMetaData->getResult() << " Nodes:\n"
                    << builtNodes.getJson(JsonOptions::none);
                JLOG(j.debug())
                    << " Valid:"
                    << " Result: " << validMetaData->getResult() << " Nodes:\n"
                    << validNodes.getJson(JsonOptions::none);
            }
            else if (index_diff)
            {
                JLOG(j.debug()) << "MISMATCH on TX " << tx
                                << ": Different index and nodes!";
                JLOG(j.debug())
                    << " Built:"
                    << " Index: " << builtMetaData->getIndex() << " Nodes:\n"
                    << builtNodes.getJson(JsonOptions::none);
                JLOG(j.debug())
                    << " Valid:"
                    << " Index: " << validMetaData->getIndex() << " Nodes:\n"
                    << validNodes.getJson(JsonOptions::none);
            }
            else  // nodes_diff
            {
                JLOG(j.debug())
                    << "MISMATCH on TX " << tx << ": Different nodes!";
                JLOG(j.debug()) << " Built:"
                                << " Nodes:\n"
                                << builtNodes.getJson(JsonOptions::none);
                JLOG(j.debug()) << " Valid:"
                                << " Nodes:\n"
                                << validNodes.getJson(JsonOptions::none);
            }
        }
    }
    else if (validMetaData != nullptr)
    {
        JLOG(j.error()) << "MISMATCH on TX " << tx
                        << ": Metadata Difference (built has none)\n"
                        << validMetaData->getJson(JsonOptions::none);
    }
    else  // builtMetaData != nullptr
    {
        JLOG(j.error()) << "MISMATCH on TX " << tx
                        << ": Metadata Difference (valid has none)\n"
                        << builtMetaData->getJson(JsonOptions::none);
    }
}

//------------------------------------------------------------------------------
void
LedgerHistory::handleMismatch(
    LedgerHash const& built,
    LedgerHash const& valid,
    std::optional<uint256> const& builtConsensusHash,
    std::optional<uint256> const& validatedConsensusHash,
    Json::Value const& consensus)
{
    assert(built != valid);
    ++mismatch_counter_;

    auto builtLedger = getLedgerByHash(built);
    auto validLedger = getLedgerByHash(valid);

    if (!builtLedger || !validLedger)
    {
        JLOG(j_.error()) << "MISMATCH cannot be analyzed:"
                         << " builtLedger: " << to_string(built) << " -> "
                         << builtLedger << " validLedger: " << to_string(valid)
                         << " -> " << validLedger;
        return;
    }

    assert(builtLedger->info().seq == validLedger->info().seq);

    if (auto stream = j_.debug())
    {
        stream << "Mismatch on " << builtLedger->info().seq << ":\n"
               << "     Built: " << getJson({*builtLedger, {}}) << "\n"
               << "     Valid: " << getJson({*validLedger, {}}) << "\n"
               << " Consensus: " << consensus;
    }

    // Determine the mismatch reason, distinguishing Byzantine
    // failure from transaction processing difference

    // Disagreement over prior ledger indicates sync issue
    if (builtLedger->info().parentHash != validLedger->info().parentHash)
    {
        JLOG(j_.error()) << "MISMATCH on prior ledger";
        return;
    }

    // Disagreement over close time indicates Byzantine failure
    if (builtLedger->info().closeTime != validLedger->info().closeTime)
    {
        JLOG(j_.error()) << "MISMATCH on close time";
        return;
    }

    if (builtConsensusHash && validatedConsensusHash)
    {
        if (builtConsensusHash != validatedConsensusHash)
            JLOG(j_.error())
                << "MISMATCH on consensus transaction set "
                << " built: " << to_string(*builtConsensusHash)
                << " validated: " << to_string(*validatedConsensusHash);
        else
            JLOG(j_.error()) << "MISMATCH with same consensus transaction set: "
                             << to_string(*builtConsensusHash);
    }

    // Grab the leaves from the specified SHAMap and sort them by key:
    auto leaves = [](SHAMap const& sm) {
        std::vector<SHAMapItem const*> v;

        for (auto const& item : sm)
            v.push_back(&item);

        std::sort(
            v.begin(),
            v.end(),
            [](SHAMapItem const* lhs, SHAMapItem const* rhs) {
                return lhs->key() < rhs->key();
            });

        return v;
    };

    // Find differences between built and valid ledgers
    auto const builtTx = leaves(builtLedger->txMap());
    auto const validTx = leaves(validLedger->txMap());

    if (builtTx == validTx)
        JLOG(j_.error()) << "MISMATCH with same " << builtTx.size()
                         << " transactions";
    else
        JLOG(j_.error()) << "MISMATCH with " << builtTx.size() << " built and "
                         << validTx.size() << " valid transactions.";

    JLOG(j_.error()) << "built\n" << getJson({*builtLedger, {}});
    JLOG(j_.error()) << "valid\n" << getJson({*validLedger, {}});

    // Log all differences between built and valid ledgers
    auto b = builtTx.begin();
    auto v = validTx.begin();
    while (b != builtTx.end() && v != validTx.end())
    {
        if ((*b)->key() < (*v)->key())
        {
            log_one(*builtLedger, (*b)->key(), "valid", j_);
            ++b;
        }
        else if ((*b)->key() > (*v)->key())
        {
            log_one(*validLedger, (*v)->key(), "built", j_);
            ++v;
        }
        else
        {
            if ((*b)->slice() != (*v)->slice())
            {
                // Same transaction with different metadata
                log_metadata_difference(
                    *builtLedger, *validLedger, (*b)->key(), j_);
            }
            ++b;
            ++v;
        }
    }
    for (; b != builtTx.end(); ++b)
        log_one(*builtLedger, (*b)->key(), "valid", j_);
    for (; v != validTx.end(); ++v)
        log_one(*validLedger, (*v)->key(), "built", j_);
}

void
LedgerHistory::builtLedger(
    std::shared_ptr<Ledger const> const& ledger,
    uint256 const& consensusHash,
    Json::Value consensus)
{
    LedgerIndex index = ledger->info().seq;
    LedgerHash hash = ledger->info().hash;
    assert(!hash.isZero());

    auto entry = std::make_shared<cv_entry>();
    consensusValidated_.retrieve_or_insert(index, entry);

    if (entry->validated && !entry->built)
    {
        if (entry->validated.value() != hash)
        {
            JLOG(j_.error()) << "MISMATCH: seq=" << index
                             << " validated:" << entry->validated.value()
                             << " then:" << hash;
            handleMismatch(
                hash,
                entry->validated.value(),
                consensusHash,
                entry->validatedConsensusHash,
                consensus);
        }
        else
        {
            // We validated a ledger and then built it locally
            JLOG(j_.debug()) << "MATCH: seq=" << index << " late";
        }
    }

    entry->built.emplace(hash);
    entry->builtConsensusHash.emplace(consensusHash);
    entry->consensus.emplace(std::move(consensus));
}

void
LedgerHistory::validatedLedger(
    std::shared_ptr<Ledger const> const& ledger,
    std::optional<uint256> const& consensusHash)
{
    LedgerIndex index = ledger->info().seq;
    LedgerHash hash = ledger->info().hash;
    assert(!hash.isZero());

    auto entry = std::make_shared<cv_entry>();
    consensusValidated_.retrieve_or_insert(index, entry);

    if (entry->built && !entry->validated && entry->built.value() != hash)
    {
        JLOG(j_.error()) << "Mismatch on validated ledger (seq " << index
                         << "): built is" << entry->built.value()
                         << ", validated is:" << hash;

        handleMismatch(
            entry->built.value(),
            hash,
            entry->builtConsensusHash,
            consensusHash,
            entry->consensus.value());
    }

    entry->validated.emplace(hash);
    entry->validatedConsensusHash = consensusHash;
}

/** Ensure ledgerCache_ doesn't have the wrong hash for a particular index
 */
bool
LedgerHistory::fixIndex(LedgerIndex ledgerIndex, LedgerHash const& ledgerHash)
{
    std::lock_guard sl(mutex_);

    if (auto it = mLedgersByIndex.find(ledgerIndex);
        (it != mLedgersByIndex.end()) && (it->second != ledgerHash))
    {
        it->second = ledgerHash;
        return false;
    }

    return true;
}

void
LedgerHistory::clearLedgerCachePrior(LedgerIndex seq)
{
    ledgerCache_.erase_if(
        [seq](Ledger const& ledger) { return ledger.info().seq < seq; });
}

Json::Value
LedgerHistory::info() const
{
    Json::Value ret(Json::objectValue);

    ret["lc"] = ledgerCache_.info();
    ret["cv"] = consensusValidated_.info();

    {
        std::lock_guard sl(mutex_);
        ret["lbi"] = std::to_string(mLedgersByIndex.size());
    }

    return ret;
}

}  // namespace ripple
