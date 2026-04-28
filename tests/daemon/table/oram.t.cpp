/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Direct unit tests for the PathORAM client (OramReadPathEviction).
 *
 * The existing cs.t.cpp / pit.t.cpp suites only exercise NFD's tables; they
 * don't read back the buffers returned by s_oram->access(), so a no-op ORAM
 * would pass them. These tests drive the ORAM client directly and assert the
 * PathORAM correctness invariant: a block written to logical address k reads
 * back identically after arbitrary unrelated traffic.
 */

#include "table/oram/Block.h"
#include "table/oram/Bucket.h"
#include "table/oram/OramInterface.h"
#include "table/oram/OramReadPathEviction.h"
#include "table/oram/RandomForOram.h"
#include "table/oram/ServerStorage.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace nfd::tests {

namespace {

constexpr int BUCKET_SIZE = 4;
constexpr int NUM_BLOCKS  = 1024; // matches Cs::ORAM_CAPACITY / Pit::ORAM_CAPACITY

struct OramFixture
{
  OramFixture()
    : storage(std::make_unique<ServerStorage>())
    , randGen(std::make_unique<RandomForOram>())
    , oram(std::make_unique<OramReadPathEviction>(storage.get(), randGen.get(),
                                                  BUCKET_SIZE, NUM_BLOCKS))
  {
  }

  // Fill a Block::BLOCK_SIZE int buffer with a deterministic pattern keyed by `seed`.
  static std::vector<int> makePattern(int seed)
  {
    std::vector<int> v(static_cast<size_t>(::Block::BLOCK_SIZE));
    for (size_t i = 0; i < v.size(); i++) {
      v[i] = seed * 1000003 + static_cast<int>(i);
    }
    return v;
  }

  std::unique_ptr<ServerStorage>          storage;
  std::unique_ptr<RandomForOram>          randGen;
  std::unique_ptr<OramReadPathEviction>   oram;
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(Table)
BOOST_FIXTURE_TEST_SUITE(TestOram, OramFixture)

// --- Construction sanity --------------------------------------------------

BOOST_AUTO_TEST_CASE(Geometry)
{
  // For NUM_BLOCKS=1024: log2(1024)+1 = 11 levels, 2^11-1 = 2047 buckets,
  // 2^10 = 1024 leaves.
  BOOST_CHECK_EQUAL(oram->getNumBlocks(), NUM_BLOCKS);
  BOOST_CHECK_EQUAL(oram->getNumLevels(), 11);
  BOOST_CHECK_EQUAL(oram->getNumBuckets(), 2047);
  BOOST_CHECK_EQUAL(oram->getNumLeaves(), 1024);
  BOOST_CHECK_EQUAL(oram->getStashSize(), 0);
}

// --- Basic write/read round-trip -----------------------------------------

BOOST_AUTO_TEST_CASE(WriteThenRead)
{
  auto pattern = makePattern(42);
  oram->access(OramInterface::WRITE, 100, pattern.data());

  std::vector<int> readBuf(static_cast<size_t>(::Block::BLOCK_SIZE), -1);
  int* got = oram->access(OramInterface::READ, 100, readBuf.data());
  BOOST_REQUIRE(got != nullptr);

  for (size_t i = 0; i < pattern.size(); i++) {
    BOOST_REQUIRE_EQUAL(got[i], pattern[i]);
  }
  delete[] got;
}

// --- Read-miss returns zeros (not NULL) ----------------------------------
// This is the new contract from our oblivious-client refactor: never expose a
// pointer-vs-data fork to the caller.

BOOST_AUTO_TEST_CASE(ReadMissReturnsZeros)
{
  std::vector<int> dummy(static_cast<size_t>(::Block::BLOCK_SIZE), 0);
  int* got = oram->access(OramInterface::READ, 7, dummy.data());
  BOOST_REQUIRE(got != nullptr);
  for (int i = 0; i < ::Block::BLOCK_SIZE; i++) {
    BOOST_CHECK_EQUAL(got[i], 0);
  }
  delete[] got;
}

// --- Position map is remapped on every access ----------------------------

BOOST_AUTO_TEST_CASE(PositionMapRemap)
{
  auto pattern = makePattern(1);
  int* pm = oram->getPositionMap();

  int leaf0 = pm[55];
  oram->access(OramInterface::WRITE, 55, pattern.data());
  int leaf1 = pm[55];
  delete[] oram->access(OramInterface::READ, 55, pattern.data());
  int leaf2 = pm[55];

  // Each access draws a fresh random leaf. With 1024 leaves the chance of
  // two consecutive draws colliding is 1/1024; we accept that and assert
  // that *at least one* of the two transitions changed the leaf.
  BOOST_CHECK(leaf0 != leaf1 || leaf1 != leaf2);
}

// --- Overwrite preserves last-writer-wins --------------------------------

BOOST_AUTO_TEST_CASE(OverwriteSameBlock)
{
  auto first  = makePattern(11);
  auto second = makePattern(22);

  oram->access(OramInterface::WRITE, 7, first.data());
  oram->access(OramInterface::WRITE, 7, second.data());

  std::vector<int> readBuf(static_cast<size_t>(::Block::BLOCK_SIZE), 0);
  int* got = oram->access(OramInterface::READ, 7, readBuf.data());
  BOOST_REQUIRE(got != nullptr);
  for (size_t i = 0; i < second.size(); i++) {
    BOOST_REQUIRE_EQUAL(got[i], second[i]);
  }
  delete[] got;
}

// --- The PathORAM correctness invariant ----------------------------------
// Write distinct payloads to N logical addresses, churn the tree with random
// unrelated accesses, then read every address back and assert the original
// payload survives.

BOOST_AUTO_TEST_CASE(ContentSurvivesChurn)
{
  constexpr int N_WRITTEN = 64;
  constexpr int N_CHURN   = 1024;

  std::unordered_map<int, std::vector<int>> truth;
  std::mt19937 rng(0xC0FFEEu);
  std::uniform_int_distribution<int> blockDist(0, NUM_BLOCKS - 1);

  // Pick N_WRITTEN distinct addresses with deterministic patterns.
  std::vector<int> addrs;
  while (static_cast<int>(addrs.size()) < N_WRITTEN) {
    int a = blockDist(rng);
    if (truth.find(a) == truth.end()) {
      addrs.push_back(a);
      truth[a] = makePattern(a);
      oram->access(OramInterface::WRITE, a, truth[a].data());
    }
  }

  // Churn: random accesses to addresses we don't care about.
  std::vector<int> scratch(static_cast<size_t>(::Block::BLOCK_SIZE), 0);
  for (int i = 0; i < N_CHURN; i++) {
    int a = blockDist(rng);
    // Skip the addresses we're tracking so the churn doesn't overwrite them.
    if (truth.count(a)) continue;
    bool isWrite = (rng() & 1u) != 0u;
    if (isWrite) {
      auto p = makePattern(a + 0x1000);
      oram->access(OramInterface::WRITE, a, p.data());
    }
    else {
      delete[] oram->access(OramInterface::READ, a, scratch.data());
    }
  }

  // Read back the tracked addresses and compare.
  for (int a : addrs) {
    std::vector<int> readBuf(static_cast<size_t>(::Block::BLOCK_SIZE), 0);
    int* got = oram->access(OramInterface::READ, a, readBuf.data());
    BOOST_REQUIRE_MESSAGE(got != nullptr, "READ returned NULL for block " << a);
    for (size_t i = 0; i < truth[a].size(); i++) {
      if (got[i] != truth[a][i]) {
        BOOST_FAIL("block " << a << " corrupted at offset " << i
                   << ": expected " << truth[a][i]
                   << " got " << got[i]);
      }
    }
    delete[] got;
  }
}

// --- Stash stays bounded under sustained traffic --------------------------
// PathORAM analysis bounds the stash to O(log N) w.h.p. With N=1024,
// bucket_size=4, expected stash is small (single-digit). We assert a generous
// upper bound; a runaway stash points to a broken eviction loop.

BOOST_AUTO_TEST_CASE(StashStaysBounded)
{
  std::mt19937 rng(0xBADBEEFu);
  std::uniform_int_distribution<int> blockDist(0, NUM_BLOCKS - 1);
  std::vector<int> scratch(static_cast<size_t>(::Block::BLOCK_SIZE), 0);

  for (int i = 0; i < 4096; i++) {
    int a = blockDist(rng);
    if ((rng() & 1u) != 0u) {
      auto p = makePattern(i);
      oram->access(OramInterface::WRITE, a, p.data());
    }
    else {
      delete[] oram->access(OramInterface::READ, a, scratch.data());
    }
  }

  int sz = oram->getStashSize();
  BOOST_TEST_MESSAGE("Final stash size: " << sz);
  // For N=1024, Z=4, 4096 ops, stash should sit comfortably below 64.
  // If this ever fires, investigate the eviction logic or the position-map
  // remap before relaxing the bound.
  BOOST_CHECK_LT(sz, 64);
}

BOOST_AUTO_TEST_SUITE_END() // TestOram
BOOST_AUTO_TEST_SUITE_END() // Table

} // namespace nfd::tests
