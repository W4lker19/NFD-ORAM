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
#include <cstring>
#include <openssl/sha.h>

namespace nfd::pit {

NFD_LOG_INIT(Pit);

std::unique_ptr<ServerStorage> Pit::s_storage;
std::unique_ptr<RandomForOram> Pit::s_randGen;
std::unique_ptr<OramInterface> Pit::s_oram;
std::once_flag Pit::s_oramOnceFlag;

static void
initOram()
{
  Pit::s_storage = std::make_unique<ServerStorage>();
  Pit::s_randGen = std::make_unique<RandomForOram>();
  Pit::s_oram = std::make_unique<OramReadPathEviction>(
    Pit::s_storage.get(), Pit::s_randGen.get(),
    /*bucket_size=*/4,
    /*num_blocks=*/Pit::ORAM_CAPACITY
  );
}

uint64_t
Pit::hashName(const Name& name)
{
  const auto& wire = name.wireEncode();
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(wire.data(),wire.size(),digest);
  uint64_t result = 0;
  memcpy(&result, digest, sizeof(result));
  return result;
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

std::pair<shared_ptr<Entry>, bool>
Pit::findOrInsert(const Interest& interest, bool allowInsert)
{
  // determine which NameTree entry should the PIT entry be attached onto
  const Name& name = interest.getName();
  bool hasDigest = name.size() > 0 && name[-1].isImplicitSha256Digest();
  size_t nteDepth = name.size() - static_cast<size_t>(hasDigest);
  nteDepth = std::min(nteDepth, NameTree::getMaxDepth());

  // ensure NameTree entry exists
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

  // check if PIT entry already exists
  const auto& pitEntries = nte->getPitEntries();
  auto it = std::find_if(pitEntries.begin(), pitEntries.end(),
    [&interest, nteDepth] (const shared_ptr<Entry>& entry) {
      // NameTree guarantees first nteDepth components are equal
      return entry->canMatch(interest, nteDepth);
    });
  if (it != pitEntries.end()) {
    int blockId = static_cast<int>(hashName(name) % ORAM_CAPACITY);
    int buf[ORAM_BLOCK_SIZE] = {};
    s_oram->access(OramInterface::READ, blockId, buf);
    NFD_LOG_DEBUG("pit-find name=" << name << " blockId=" << blockId);
    return {*it, false};
  }

  if (!allowInsert) {
    BOOST_ASSERT(!nte->isEmpty()); // nte shouldn't be created in this call
    return {nullptr, true};
  }

  int blockId = static_cast<int>(hashName(name) % ORAM_CAPACITY);
  int buf[ORAM_BLOCK_SIZE] = {};
  buf[0] = 1;
  buf[1] = blockId;
  const auto& wire = name.wireEncode();
  memcpy(&buf[2], wire.data(), std::min(wire.size(), static_cast<size_t>((ORAM_BLOCK_SIZE - 2) * sizeof(int))));
  s_oram->access(OramInterface::WRITE, blockId, buf);
  NFD_LOG_INFO("pit-insert blockId=" << blockId << " name=" << name);


  auto entry = make_shared<Entry>(interest);
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
  const Name& name = entry->getInterest().getName();
  int blockId = static_cast<int>(hashName(name) % ORAM_CAPACITY);
  int buf[ORAM_BLOCK_SIZE] = {};
  s_oram->access(OramInterface::WRITE, blockId, buf);


  name_tree::Entry* nte = m_nameTree.getEntry(*entry);
  BOOST_ASSERT(nte != nullptr);

  NFD_LOG_DEBUG("pit-erase blockId=" << blockId << " name=" << name);

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

  /// \todo decide whether to delete PIT entry if there's no more in/out-record left
}

Pit::const_iterator
Pit::begin() const
{
  return const_iterator(m_nameTree.fullEnumerate(&nteHasPitEntries).begin());
}

} // namespace nfd::pit
