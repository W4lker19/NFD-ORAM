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

#ifndef NFD_DAEMON_TABLE_CS_HPP
#define NFD_DAEMON_TABLE_CS_HPP

#include "cs-policy.hpp"
#include "oram/OramInterface.h"
#include "oram/OramReadPathEviction.h"
#include "oram/ServerStorage.h"
#include "oram/RandomForOram.h"

#include <deque>
#include <mutex>

namespace nfd
{
  namespace cs
  {

    class Cs : noncopyable
    {
    public:
      explicit Cs(size_t nMaxPackets = 10);

      ~Cs();

      void
      insert(const Data &data, bool isUnsolicited = false);

      template <typename AfterEraseCallback>
      void
      erase(const Name &prefix, size_t limit, AfterEraseCallback &&cb)
      {
        size_t nErased = eraseImpl(prefix, limit);
        cb(nErased);
      }

      template <typename HitCallback, typename MissCallback>
      void
      find(const Interest &interest, HitCallback &&hit, MissCallback &&miss) const
      {
        // ORAM is the source of truth for the Data wire bytes; m_table only
        // holds the index. findImpl returns both the iterator (for policy
        // bookkeeping) and the freshly reconstructed Data.
        shared_ptr<Data> matched;
        auto it = findImpl(interest, matched);
        if (it == m_table.end() || matched == nullptr) {
          miss(interest);
          return;
        }
        hit(interest, *matched);
      }

      size_t
      size() const
      {
        return m_table.size();
      }

    public: // configuration
      size_t
      getLimit() const noexcept
      {
        return m_policy->getLimit();
      }

      void
      setLimit(size_t nMaxPackets)
      {
        return m_policy->setLimit(nMaxPackets);
      }

      Policy *
      getPolicy() const noexcept
      {
        return m_policy.get();
      }

      void
      setPolicy(unique_ptr<Policy> policy);

      bool
      shouldAdmit() const noexcept
      {
        return m_shouldAdmit;
      }

      void
      enableAdmit(bool shouldAdmit) noexcept;

      bool
      shouldServe() const noexcept
      {
        return m_shouldServe;
      }

      void
      enableServe(bool shouldServe) noexcept;

    public: // enumeration
      using const_iterator = Table::const_iterator;

      const_iterator
      begin() const
      {
        return m_table.begin();
      }

      const_iterator
      end() const
      {
        return m_table.end();
      }

    private:
      std::pair<const_iterator, const_iterator>
      findPrefixRange(const Name &prefix) const;

      size_t
      eraseImpl(const Name &prefix, size_t limit);

      // findImpl now returns both the index iterator and the Data
      // reconstructed from the ORAM block (or nullptr on miss / decode failure).
      const_iterator
      findImpl(const Interest &interest, shared_ptr<Data>& outData) const;

      const_iterator
      findInOram(const Interest &interest, shared_ptr<Data>& outData) const;

      // Read ORAM block `blockId` and reconstruct the encoded Data.
      // Returns nullptr if the block is empty or the wire bytes don't decode.
      static shared_ptr<Data>
      readDataFromOram(int blockId);

      // Write `data`'s wire encoding into ORAM block `blockId`.
      // Returns false if the encoded data exceeds the per-block budget.
      static bool
      writeDataToOram(int blockId, const Data& data);

      // Zero ORAM block `blockId` (erasure / eviction).
      static void
      clearOramBlock(int blockId);

      // blockId allocator: pop a free id, or return -1 if pool is empty.
      static int
      allocateBlockId();

      // Return `blockId` to the free pool.
      static void
      freeBlockId(int blockId);

      void
      setPolicyImpl(unique_ptr<Policy> policy);

    private:
      Table m_table;
      unique_ptr<Policy> m_policy;
      signal::ScopedConnection m_beforeEvictConnection;

      bool m_shouldAdmit = true;
      bool m_shouldServe = true;

    public: // ORAM
      static constexpr int ORAM_CAPACITY = 4096;
      // Tied to Block::BLOCK_SIZE — see note in pit.hpp.
      static constexpr int ORAM_BLOCK_SIZE = ::Block::BLOCK_SIZE;
      static std::unique_ptr<ServerStorage> s_storage;
      static std::unique_ptr<RandomForOram> s_randGen;
      static std::unique_ptr<OramInterface> s_oram;
      static std::once_flag s_oramOnceFlag;
      static std::deque<int> s_freeBlockIds;
      static std::mutex s_oramMutex;
    };

  } // namespace cs

  using cs::Cs;

} // namespace nfd

#endif // NFD_DAEMON_TABLE_CS_HPP