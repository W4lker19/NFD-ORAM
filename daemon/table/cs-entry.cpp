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

#include "cs-entry.hpp"

namespace nfd::cs {

Entry::Entry(const Data& data, int blockId, bool isUnsolicited)
  : m_name(data.getName())
  , m_fullName(data.getFullName())
  , m_freshnessPeriod(data.getFreshnessPeriod())
  , m_blockId(blockId)
  , m_isUnsolicited(isUnsolicited)
{
  updateFreshUntil();
}

bool
Entry::isFresh() const
{
  return m_freshUntil >= time::steady_clock::now();
}

void
Entry::updateFreshUntil()
{
  m_freshUntil = time::steady_clock::now() + m_freshnessPeriod;
}

// Same lookup semantics as the original, but driven from the cached Names
// instead of the (now-removed) m_data. queryName may or may not include an
// implicit-digest component; the entry always stores both forms.
static int
compareQueryWithEntry(const Name& queryName, const Entry& entry)
{
  bool queryIsFullName = !queryName.empty() && queryName[-1].isImplicitSha256Digest();

  int cmp = queryIsFullName ?
            queryName.compare(0, queryName.size() - 1, entry.getName()) :
            queryName.compare(entry.getName());

  if (cmp != 0) {
    return cmp;
  }

  if (queryIsFullName) {
    return queryName[-1].compare(entry.getFullName()[-1]);
  }
  // queryName is a proper prefix of entry's full name
  return -1;
}

bool
operator<(const Entry& entry, const Name& queryName)
{
  return compareQueryWithEntry(queryName, entry) > 0;
}

bool
operator<(const Name& queryName, const Entry& entry)
{
  return compareQueryWithEntry(queryName, entry) < 0;
}

bool
operator<(const Entry& lhs, const Entry& rhs)
{
  int cmp = lhs.getName().compare(rhs.getName());
  if (cmp != 0) {
    return cmp < 0;
  }
  return lhs.getFullName()[-1].compare(rhs.getFullName()[-1]) < 0;
}

} // namespace nfd::cs
