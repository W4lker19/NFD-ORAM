/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2025,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "cs.hpp"
#include "common/logger.hpp"
#include <ndn-cxx/lp/tags.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <cstring>

namespace nfd::cs {

NFD_LOG_INIT(ContentStore);

// ORAM static members
std::unique_ptr<ServerStorage>  Cs::s_storage;
std::unique_ptr<RandomForOram>  Cs::s_randGen;
std::unique_ptr<OramInterface>  Cs::s_oram;
std::once_flag                  Cs::s_oramOnceFlag;
std::deque<int>                 Cs::s_freeBlockIds;
std::mutex                      Cs::s_oramMutex;

// Layout of an ORAM block (Block::BLOCK_SIZE ints, 8800 B):
//   buf[0]            valid flag (1 = real, 0 = empty / erased)
//   buf[1]            blockId (sanity check; redundant)
//   bytes 8..end      TLV-self-delimited Data wire encoding
namespace {
constexpr int  BLOCK_VALID_OFFSET = 0;
constexpr int  BLOCK_ID_OFFSET    = 1;
constexpr int  WIRE_INT_OFFSET    = 2;
constexpr size_t WIRE_BYTE_OFFSET = WIRE_INT_OFFSET * sizeof(int);
constexpr size_t MAX_WIRE_BYTES   = (Cs::ORAM_BLOCK_SIZE - WIRE_INT_OFFSET) * sizeof(int);
} // anonymous namespace

static unique_ptr<Policy>
makeDefaultPolicy()
{
  return Policy::create("lru");
}

static void
initOram()
{
  Cs::s_storage = std::make_unique<ServerStorage>();
  Cs::s_randGen = std::make_unique<RandomForOram>();
  Cs::s_oram = std::make_unique<OramReadPathEviction>(
      Cs::s_storage.get(), Cs::s_randGen.get(),
      /*bucket_size=*/4,
      /*num_blocks=*/Cs::ORAM_CAPACITY);

  Cs::s_freeBlockIds.clear();
  for (int i = 0; i < Cs::ORAM_CAPACITY; i++) {
    Cs::s_freeBlockIds.push_back(i);
  }
}

int
Cs::allocateBlockId()
{
  std::lock_guard<std::mutex> lk(s_oramMutex);
  if (s_freeBlockIds.empty()) {
    return -1;
  }
  int id = s_freeBlockIds.front();
  s_freeBlockIds.pop_front();
  return id;
}

void
Cs::freeBlockId(int blockId)
{
  std::lock_guard<std::mutex> lk(s_oramMutex);
  s_freeBlockIds.push_back(blockId);
}

// Returns true on success. Caller must hold the blockId allocation; on
// failure the caller is responsible for freeing it.
bool
Cs::writeDataToOram(int blockId, const Data& data)
{
  const auto& wire = data.wireEncode();
  if (wire.size() > MAX_WIRE_BYTES) {
    NFD_LOG_WARN("oram-insert wire too large (" << wire.size()
                 << " > " << MAX_WIRE_BYTES << "), refusing block " << blockId);
    return false;
  }

  int buf[ORAM_BLOCK_SIZE] = {};
  buf[BLOCK_VALID_OFFSET] = 1;
  buf[BLOCK_ID_OFFSET]    = blockId;
  std::memcpy(reinterpret_cast<uint8_t*>(buf) + WIRE_BYTE_OFFSET,
              wire.data(), wire.size());

  std::lock_guard<std::mutex> lk(s_oramMutex);
  s_oram->access(OramInterface::WRITE, blockId, buf);
  return true;
}

shared_ptr<Data>
Cs::readDataFromOram(int blockId)
{
  // For READ, OramReadPathEviction::access does not write to the supplied
  // buffer — it allocates and returns a fresh int[] containing the block
  // contents. We pass a dummy buffer to satisfy the API.
  int dummy[ORAM_BLOCK_SIZE] = {};
  int* raw;
  {
    std::lock_guard<std::mutex> lk(s_oramMutex);
    raw = s_oram->access(OramInterface::READ, blockId, dummy);
  }
  // OramReadPathEviction::access returns a heap-allocated buffer that is
  // a copy of the block on hit and zeroed on miss. We own it.
  std::unique_ptr<int[]> owned(raw);

  if (owned == nullptr || owned[BLOCK_VALID_OFFSET] != 1) {
    return nullptr;
  }

  const uint8_t* wireStart = reinterpret_cast<const uint8_t*>(owned.get())
                             + WIRE_BYTE_OFFSET;
  try {
    auto [isOk, element] = ndn::Block::fromBuffer({wireStart, MAX_WIRE_BYTES});
    if (!isOk || element.type() != ndn::tlv::Data) {
      NFD_LOG_WARN("oram-read decode failed for blockId=" << blockId);
      return nullptr;
    }
    return std::make_shared<Data>(element);
  }
  catch (const ndn::tlv::Error& e) {
    NFD_LOG_WARN("oram-read TLV error for blockId=" << blockId << ": " << e.what());
    return nullptr;
  }
}

void
Cs::clearOramBlock(int blockId)
{
  int buf[ORAM_BLOCK_SIZE] = {}; // zeros = empty
  std::lock_guard<std::mutex> lk(s_oramMutex);
  s_oram->access(OramInterface::WRITE, blockId, buf);
}

Cs::Cs(size_t nMaxPackets)
{
  std::call_once(s_oramOnceFlag, initOram);
  setPolicyImpl(makeDefaultPolicy());
  // Cap the policy limit at ORAM_CAPACITY: the blockId pool has exactly that
  // many slots, so allowing more table entries would let inserts fail silently
  // when the pool drains.
  m_policy->setLimit(std::min(nMaxPackets, static_cast<size_t>(ORAM_CAPACITY)));
}

Cs::~Cs()
{
  // Return every blockId owned by this CS to the shared pool so a subsequent
  // Cs (e.g. across unit tests) starts with a full free list. The ORAM
  // buckets keep stale ciphertext, but they get overwritten on next
  // allocation and the index records are gone — so the leftover bytes are
  // unreachable.
  for (auto it = m_table.begin(); it != m_table.end(); ++it) {
    clearOramBlock(it->getBlockId());
    freeBlockId(it->getBlockId());
  }
}

void
Cs::insert(const Data& data, bool isUnsolicited)
{
  if (data.getName().size() > 0 &&
      data.getName().get(0) == Name("/localhost")[0])
    return;

  if (!m_shouldAdmit || m_policy->getLimit() == 0)
    return;

  NFD_LOG_DEBUG("insert " << data.getName());

  auto tag = data.getTag<ndn::lp::CachePolicyTag>();
  if (tag != nullptr) {
    if (tag->get().getPolicy() == ndn::lp::CachePolicyType::NO_CACHE)
      return;
  }

  // Probe whether this exact (name + digest) already exists by full-name lookup.
  Name fullName = data.getFullName();
  auto existing = m_table.find(fullName);
  if (existing != m_table.end()) {
    // Refresh in place — same blockId, rewrite ORAM block to refresh contents.
    auto& entry = const_cast<Entry&>(*existing);
    entry.updateFreshUntil();
    if (entry.isUnsolicited() && !isUnsolicited) {
      entry.clearUnsolicited();
    }
    writeDataToOram(entry.getBlockId(), data);
    m_policy->afterRefresh(existing);
    return;
  }

  // New entry: allocate a blockId, write to ORAM, insert into the index.
  int blockId = allocateBlockId();
  if (blockId < 0) {
    NFD_LOG_WARN("insert: ORAM blockId pool exhausted, dropping " << data.getName());
    return;
  }

  if (!writeDataToOram(blockId, data)) {
    freeBlockId(blockId);
    return;
  }
  NFD_LOG_INFO("oram-insert blockId=" << blockId << " name=" << data.getFullName());

  auto [it, inserted] = m_table.emplace(data, blockId, isUnsolicited);
  if (!inserted) {
    // Should not happen — we just checked existence above. Defensive cleanup.
    clearOramBlock(blockId);
    freeBlockId(blockId);
    return;
  }
  m_policy->afterInsert(it);
}

std::pair<Cs::const_iterator, Cs::const_iterator>
Cs::findPrefixRange(const Name& prefix) const
{
  auto first = m_table.lower_bound(prefix);
  auto last  = m_table.end();
  if (!prefix.empty())
    last = m_table.lower_bound(prefix.getSuccessor());
  return {first, last};
}

size_t
Cs::eraseImpl(const Name& prefix, size_t limit)
{
  const_iterator i, last;
  std::tie(i, last) = findPrefixRange(prefix);

  size_t nErased = 0;
  while (i != last && nErased < limit) {
    int blockId = i->getBlockId();
    NFD_LOG_DEBUG("oram-erase blockId=" << blockId << " name=" << i->getFullName());
    clearOramBlock(blockId);
    freeBlockId(blockId);

    m_policy->beforeErase(i);
    i = m_table.erase(i);
    ++nErased;
  }
  return nErased;
}

Cs::const_iterator
Cs::findInOram(const Interest& interest, shared_ptr<Data>& outData) const
{
  auto it = m_table.lower_bound(interest.getName());
  if (it == m_table.end()) {
    return m_table.end();
  }

  // Read the candidate's ORAM block and reconstruct the Data.
  auto data = readDataFromOram(it->getBlockId());
  if (data == nullptr) {
    NFD_LOG_WARN("findInOram: empty/corrupt block for " << it->getFullName());
    return m_table.end();
  }

  // Run the original canSatisfy check against the freshly decoded Data.
  if (!interest.matchesData(*data)) {
    return m_table.end();
  }
  if (interest.getMustBeFresh() && !it->isFresh()) {
    return m_table.end();
  }

  NFD_LOG_INFO("find " << interest.getName()
               << " matched " << it->getFullName()
               << " blockId=" << it->getBlockId());
  outData = std::move(data);
  m_policy->beforeUse(it);
  return it;
}

Cs::const_iterator
Cs::findImpl(const Interest& interest, shared_ptr<Data>& outData) const
{
  if (!m_shouldServe || m_policy->getLimit() == 0)
    return m_table.end();

  auto result = findInOram(interest, outData);
  if (result == m_table.end()) {
    NFD_LOG_INFO("find " << interest.getName() << " no-match");
  }
  return result;
}

void
Cs::setPolicy(unique_ptr<Policy> policy)
{
  BOOST_ASSERT(policy != nullptr);
  BOOST_ASSERT(m_policy != nullptr);
  size_t limit = m_policy->getLimit();
  this->setPolicyImpl(std::move(policy));
  m_policy->setLimit(limit);
}

void
Cs::setPolicyImpl(unique_ptr<Policy> policy)
{
  NFD_LOG_DEBUG("set-policy " << policy->getName());
  m_policy = std::move(policy);

  m_beforeEvictConnection = m_policy->beforeEvict.connect([this](auto it) {
    int blockId = it->getBlockId();
    NFD_LOG_DEBUG("oram-evict blockId=" << blockId << " name=" << it->getFullName());
    clearOramBlock(blockId);
    freeBlockId(blockId);
    m_table.erase(it);
  });

  m_policy->setCs(this);
  BOOST_ASSERT(m_policy->getCs() == this);
}

void
Cs::enableAdmit(bool shouldAdmit) noexcept
{
  if (m_shouldAdmit == shouldAdmit)
    return;
  m_shouldAdmit = shouldAdmit;
  NFD_LOG_INFO((shouldAdmit ? "Enabling" : "Disabling") << " Data admittance");
}

void
Cs::enableServe(bool shouldServe) noexcept
{
  if (m_shouldServe == shouldServe)
    return;
  m_shouldServe = shouldServe;
  NFD_LOG_INFO((shouldServe ? "Enabling" : "Disabling") << " Data serving");
}

} // namespace nfd::cs
