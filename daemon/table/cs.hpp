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

namespace nfd
{
  namespace cs
  {

    class Cs : noncopyable
    {
    public:
      explicit Cs(size_t nMaxPackets = 10);

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
        auto match = findImpl(interest);
        if (match == m_table.end())
        {
          miss(interest);
          return;
        }
        hit(interest, match->getData());
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

      const_iterator
      findImpl(const Interest &interest) const;

      static uint64_t
      hashPrefix(const Name& name);

      const_iterator
      findInOram(const Interest &interest) const;

      void
      setPolicyImpl(unique_ptr<Policy> policy);

    private:
      Table m_table;
      unique_ptr<Policy> m_policy;
      signal::ScopedConnection m_beforeEvictConnection;

      bool m_shouldAdmit = true;
      bool m_shouldServe = true;

      // ORAM index: blockId -> iterator into m_table
    public: // ORAM
      static constexpr int ORAM_CAPACITY = 1024;
      // Tied to Block::BLOCK_SIZE — see note in pit.hpp.
      static constexpr int ORAM_BLOCK_SIZE = Block::BLOCK_SIZE;
      static std::unique_ptr<ServerStorage> s_storage;
      static std::unique_ptr<RandomForOram> s_randGen;
      static std::unique_ptr<OramInterface> s_oram;
      static std::once_flag s_oramOnceFlag;
    };

  } // namespace cs

  using cs::Cs;

} // namespace nfd

#endif // NFD_DAEMON_TABLE_CS_HPP