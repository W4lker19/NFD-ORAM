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

#ifndef NFD_DAEMON_TABLE_CS_ENTRY_HPP
#define NFD_DAEMON_TABLE_CS_ENTRY_HPP

#include "core/common.hpp"

#include <set>

namespace nfd::cs {

/** \brief A ContentStore entry.
 *
 *  After the ORAM-authoritative refactor, the Entry is a *thin index record*:
 *  it holds the bare and full Names of the cached Data plus enough metadata
 *  for freshness/policy decisions. The encoded Data wire bytes themselves
 *  live in the ORAM and are reconstructed on every read via Cs::find.
 */
class Entry
{
public: // exposed through ContentStore enumeration

  /** \brief Return stored Data name (without implicit digest).
   */
  const Name&
  getName() const
  {
    return m_name;
  }

  /** \brief Return full name (including implicit digest) of the stored Data.
   */
  const Name&
  getFullName() const
  {
    return m_fullName;
  }

  /** \brief Return whether the stored Data is unsolicited.
   */
  bool
  isUnsolicited() const
  {
    return m_isUnsolicited;
  }

  /** \brief Check if the stored Data is fresh now.
   */
  bool
  isFresh() const;

  /** \brief Freshness period of the stored Data.
   *
   *  Cached at insert time so policies (e.g. priority-fifo) can schedule
   *  staleness without re-reading the ORAM block.
   */
  time::milliseconds
  getFreshnessPeriod() const
  {
    return m_freshnessPeriod;
  }

  /** \brief ORAM block id where the encoded Data lives.
   */
  int
  getBlockId() const
  {
    return m_blockId;
  }

public: // used by ContentStore implementation

  /** \brief Build an index entry for \p data, addressing ORAM block \p blockId.
   */
  Entry(const Data& data, int blockId, bool isUnsolicited);

  /** \brief Recalculate when the entry would become non-fresh, relative to current time.
   */
  void
  updateFreshUntil();

  /** \brief Clear 'unsolicited' flag.
   */
  void
  clearUnsolicited()
  {
    m_isUnsolicited = false;
  }

private:
  Name m_name;
  Name m_fullName;
  time::milliseconds m_freshnessPeriod;
  time::steady_clock::time_point m_freshUntil;
  int m_blockId;
  bool m_isUnsolicited;
};

bool
operator<(const Entry& entry, const Name& queryName);

bool
operator<(const Name& queryName, const Entry& entry);

bool
operator<(const Entry& lhs, const Entry& rhs);

/** \brief An ordered container of ContentStore entries.
 *
 *  This container uses std::less<> comparator to enable lookup with queryName.
 */
using Table = std::set<Entry, std::less<>>;

inline bool
operator<(Table::const_iterator lhs, Table::const_iterator rhs)
{
  return *lhs < *rhs;
}

} // namespace nfd::cs

#endif // NFD_DAEMON_TABLE_CS_ENTRY_HPP
