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
#include <cstring>
#include <openssl/sha.h>

namespace nfd::cs {

NFD_LOG_INIT(ContentStore);

// ORAM static members
std::unique_ptr<ServerStorage>  Cs::s_storage;
std::unique_ptr<RandomForOram>  Cs::s_randGen;
std::unique_ptr<OramInterface>  Cs::s_oram;
std::once_flag  Cs::s_oramOnceFlag;

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
}

uint64_t
Cs::hashPrefix(const Name& name)
{
  const auto& wire = name.wireEncode();
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(wire.data(), wire.size(), digest);

  uint64_t result = 0;
  memcpy(&result, digest, sizeof(result));
  return result;
}

Cs::Cs(size_t nMaxPackets)
{
  std::call_once(s_oramOnceFlag, initOram);
  setPolicyImpl(makeDefaultPolicy());
  m_policy->setLimit(nMaxPackets);
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

  auto [it, isNewEntry] = m_table.emplace(data.shared_from_this(), isUnsolicited);
  auto& entry = const_cast<Entry&>(*it);
  entry.updateFreshUntil();

  if (!isNewEntry) {
    if (entry.isUnsolicited() && !isUnsolicited)
      entry.clearUnsolicited();
    m_policy->afterRefresh(it);
  }
  else {
    const Name& fullName = data.getName();

    // blockId derivado directamente do hash do nome — sem índice auxiliar
    int blockId = static_cast<int>(hashPrefix(fullName) % ORAM_CAPACITY);

    int buf[ORAM_BLOCK_SIZE] = {};
    buf[0] = 1;       // flag válido
    buf[1] = blockId;
    const auto& wire = data.wireEncode();
    memcpy(&buf[2], wire.data(),
           std::min(wire.size(),
                    static_cast<size_t>((ORAM_BLOCK_SIZE - 2) * sizeof(int))));

    s_oram->access(OramInterface::WRITE, blockId, buf);
    NFD_LOG_INFO("oram-insert blockId=" << blockId << " name=" << fullName);

    m_policy->afterInsert(it);
  }
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
    const Name& erasedName = i->getData().getName();
    int blockId = static_cast<int>(hashPrefix(erasedName) % ORAM_CAPACITY);

    int buf[ORAM_BLOCK_SIZE] = {};  // zeros = apagado
    s_oram->access(OramInterface::WRITE, blockId, buf);

    m_policy->beforeErase(i);
    i = m_table.erase(i);
    ++nErased;
  }
  return nErased;
}

Cs::const_iterator
Cs::findInOram(const Interest& interest) const
{
  auto it = m_table.lower_bound(interest.getName());
  if (it != m_table.end() && it->canSatisfy(interest)) {
    const Name& fullName = it->getData().getName();
    int blockId = static_cast<int>(hashPrefix(fullName) % ORAM_CAPACITY);

    int buf[ORAM_BLOCK_SIZE] = {};
    s_oram->access(OramInterface::READ, blockId, buf);

    NFD_LOG_INFO("find " << interest.getName()
                 << " matched " << fullName
                 << " blockId=" << blockId);
    m_policy->beforeUse(it);
    return it;
  }

  return m_table.end();
}

Cs::const_iterator
Cs::findImpl(const Interest& interest) const
{
  if (!m_shouldServe || m_policy->getLimit() == 0)
    return m_table.end();

  auto result = findInOram(interest);
  if (result == m_table.end())
    NFD_LOG_INFO("find " << interest.getName() << " no-match");
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
    const Name& evictedName = it->getData().getName();
    int blockId = static_cast<int>(hashPrefix(evictedName) % ORAM_CAPACITY);

    int buf[ORAM_BLOCK_SIZE] = {};  // zeros = apagado
    s_oram->access(OramInterface::WRITE, blockId, buf);

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
