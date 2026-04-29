/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2024,  Regents of the University of California,
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

#include "pit.hpp"
#include "common/logger.hpp"
#include <ndn-cxx/encoding/block.hpp>
#include <cstring>

namespace nfd::pit {

NFD_LOG_INIT(Pit);

std::unique_ptr<ServerStorage> Pit::s_storage;
std::unique_ptr<RandomForOram> Pit::s_randGen;
std::unique_ptr<OramInterface>  Pit::s_oram;
std::once_flag                  Pit::s_oramOnceFlag;
std::deque<int>                 Pit::s_freeBlockIds;
std::mutex                      Pit::s_oramMutex;

// Block layout (same convention as CS):
//   buf[0]       valid flag (1 = real, 0 = empty)
//   buf[1]       blockId (sanity check)
//   bytes[8..]   TLV-self-delimited Interest wire encoding
namespace {
constexpr int    BLOCK_VALID_OFFSET = 0;
constexpr int    BLOCK_ID_OFFSET    = 1;
constexpr int    WIRE_INT_OFFSET    = 2;
constexpr size_t WIRE_BYTE_OFFSET   = WIRE_INT_OFFSET * sizeof(int);
constexpr size_t MAX_WIRE_BYTES     = (Pit::ORAM_BLOCK_SIZE - WIRE_INT_OFFSET) * sizeof(int);
} // anonymous namespace

static void
initOram()
{
  Pit::s_storage = std::make_unique<ServerStorage>();
  Pit::s_randGen = std::make_unique<RandomForOram>();
  Pit::s_oram    = std::make_unique<OramReadPathEviction>(
    Pit::s_storage.get(), Pit::s_randGen.get(),
    /*bucket_size=*/4,
    /*num_blocks=*/Pit::ORAM_CAPACITY);

  Pit::s_freeBlockIds.clear();
  for (int i = 0; i < Pit::ORAM_CAPACITY; i++) {
    Pit::s_freeBlockIds.push_back(i);
  }
}

int
Pit::allocateBlockId()
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
Pit::freeBlockId(int blockId)
{
  std::lock_guard<std::mutex> lk(s_oramMutex);
  s_freeBlockIds.push_back(blockId);
}

bool
Pit::writeInterestToOram(int blockId, const Interest& interest)
{
  const auto& wire = interest.wireEncode();
  if (wire.size() > MAX_WIRE_BYTES) {
    NFD_LOG_WARN("pit-insert wire too large (" << wire.size()
                 << " > " << MAX_WIRE_BYTES << ") for blockId=" << blockId);
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

shared_ptr<const Interest>
Pit::readInterestFromOram(int blockId)
{
  int dummy[ORAM_BLOCK_SIZE] = {};
  int* raw;
  {
    std::lock_guard<std::mutex> lk(s_oramMutex);
    raw = s_oram->access(OramInterface::READ, blockId, dummy);
  }
  std::unique_ptr<int[]> owned(raw);

  if (owned == nullptr || owned[BLOCK_VALID_OFFSET] != 1) {
    return nullptr;
  }

  const uint8_t* wireStart = reinterpret_cast<const uint8_t*>(owned.get())
                             + WIRE_BYTE_OFFSET;
  try {
    auto [isOk, element] = ndn::Block::fromBuffer({wireStart, MAX_WIRE_BYTES});
    if (!isOk || element.type() != ndn::tlv::Interest) {
      NFD_LOG_WARN("pit-find decode failed for blockId=" << blockId);
      return nullptr;
    }
    return std::make_shared<const Interest>(element);
  }
  catch (const ndn::tlv::Error& e) {
    NFD_LOG_WARN("pit-find TLV error for blockId=" << blockId << ": " << e.what());
    return nullptr;
  }
}

void
Pit::clearOramBlock(int blockId)
{
  int buf[ORAM_BLOCK_SIZE] = {};
  std::lock_guard<std::mutex> lk(s_oramMutex);
  s_oram->access(OramInterface::WRITE, blockId, buf);
}

Iterator&
Iterator::operator++()
{
  BOOST_ASSERT(m_ntIt != NameTree::const_iterator());
  BOOST_ASSERT(m_iPitEntry < m_ntIt->getPitEntries().size());

  if (++m_iPitEntry >= m_ntIt->getPitEntries().size()) {
    ++m_ntIt;
    m_iPitEntry = 0;
    BOOST_ASSERT(m_ntIt == NameTree::const_iterator() || m_ntIt->hasPitEntries());
  }
  return *this;
}

Pit::Pit(NameTree& nameTree)
  : m_nameTree(nameTree)
{
  std::call_once(s_oramOnceFlag, initOram);
}

Pit::~Pit()
{
  // Release all blockIds owned by this Pit back to the shared pool so a
  // subsequent Pit (e.g. across unit-test fixtures) starts with a full list.
  for (const auto& entry : *this) {
    int id = entry.getBlockId();
    if (id >= 0) {
      clearOramBlock(id);
      freeBlockId(id);
    }
  }
}

std::pair<shared_ptr<Entry>, bool>
Pit::findOrInsert(const Interest& interest, bool allowInsert)
{
  const Name& name = interest.getName();
  bool hasDigest = name.size() > 0 && name[-1].isImplicitSha256Digest();
  size_t nteDepth = name.size() - static_cast<size_t>(hasDigest);
  nteDepth = std::min(nteDepth, NameTree::getMaxDepth());

  name_tree::Entry* nte = nullptr;
  if (allowInsert) {
    nte = &m_nameTree.lookup(name, nteDepth);
  }
  else {
    nte = m_nameTree.findExactMatch(name, nteDepth);
    if (nte == nullptr) {
      return {nullptr, true};
    }
  }

  const auto& pitEntries = nte->getPitEntries();
  auto it = std::find_if(pitEntries.begin(), pitEntries.end(),
    [&interest, nteDepth](const shared_ptr<Entry>& entry) {
      return entry->canMatch(interest, nteDepth);
    });

  if (it != pitEntries.end()) {
    // Existing entry: ORAM-read the Interest and refresh the in-memory copy
    // so the forwarding pipeline always sees data sourced from the ORAM.
    shared_ptr<Entry> entry = *it;
    int blockId = entry->getBlockId();
    if (blockId >= 0) {
      auto decoded = readInterestFromOram(blockId);
      if (decoded != nullptr) {
        entry->refreshInterest(std::move(decoded));
      }
      else {
        NFD_LOG_WARN("pit-find: ORAM read failed for blockId=" << blockId
                     << " name=" << name);
      }
    }
    NFD_LOG_DEBUG("pit-find name=" << name << " blockId=" << blockId);
    return {entry, false};
  }

  if (!allowInsert) {
    BOOST_ASSERT(!nte->isEmpty());
    return {nullptr, true};
  }

  // New entry: allocate a blockId, write Interest to ORAM, attach to NameTree.
  int blockId = allocateBlockId();
  if (blockId < 0) {
    NFD_LOG_WARN("pit-insert: ORAM blockId pool exhausted, dropping " << name);
    return {nullptr, true};
  }

  if (!writeInterestToOram(blockId, interest)) {
    freeBlockId(blockId);
    return {nullptr, true};
  }
  NFD_LOG_INFO("pit-insert blockId=" << blockId << " name=" << name);

  auto entry = make_shared<Entry>(interest);
  entry->setBlockId(blockId);
  nte->insertPitEntry(entry);
  ++m_nItems;
  return {entry, true};
}

static bool
nteHasPitEntries(const name_tree::Entry& nte)
{
  return nte.hasPitEntries();
}

DataMatchResult
Pit::findAllDataMatches(const Data& data) const
{
  auto&& ntMatches = m_nameTree.findAllMatches(data.getName(), &nteHasPitEntries);

  DataMatchResult matches;
  for (const auto& nte : ntMatches) {
    for (const auto& pitEntry : nte.getPitEntries()) {
      if (pitEntry->getInterest().matchesData(data))
        matches.emplace_back(pitEntry);
    }
  }

  return matches;
}

void
Pit::erase(Entry* entry, bool canDeleteNte)
{
  int blockId = entry->getBlockId();
  if (blockId >= 0) {
    clearOramBlock(blockId);
    freeBlockId(blockId);
  }

  name_tree::Entry* nte = m_nameTree.getEntry(*entry);
  BOOST_ASSERT(nte != nullptr);

  NFD_LOG_DEBUG("pit-erase blockId=" << blockId
                << " name=" << entry->getInterest().getName());

  nte->erasePitEntry(entry);
  if (canDeleteNte) {
    m_nameTree.eraseIfEmpty(nte);
  }
  --m_nItems;
}

void
Pit::deleteInOutRecords(Entry* entry, const Face& face)
{
  BOOST_ASSERT(entry != nullptr);

  auto in = entry->findInRecord(face);
  if (in != entry->in_end()) {
    entry->deleteInRecord(in);
  }
  entry->deleteOutRecord(face);
}

Pit::const_iterator
Pit::begin() const
{
  return const_iterator(m_nameTree.fullEnumerate(&nteHasPitEntries).begin());
}

} // namespace nfd::pit
