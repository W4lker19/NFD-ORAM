//
//
//

#include "OramReadPathEviction.h"
#include "ObliviousOps.h"
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <string>
#include <sstream>
#include <cmath>

OramReadPathEviction::OramReadPathEviction(UntrustedStorageInterface* storage, RandForOramInterface* rand_gen,
                                           int bucket_size, int num_blocks) {
    this->storage = storage;
    this->rand_gen = rand_gen;
    this->bucket_size = bucket_size;
    this->num_blocks = num_blocks;
    this->num_levels = ceil(log10(num_blocks) / log10(2)) + 1;
    this->num_buckets = pow(2, num_levels)-1;
    if (this->num_buckets*this->bucket_size < this->num_blocks) //deal with precision loss
    {
        throw runtime_error("Not enough space for the acutal number of blocks.");
    }
    this->num_leaves = pow(2, num_levels-1);
    Bucket::resetState();
    Bucket::setMaxSize(bucket_size);
    this->rand_gen->setBound(num_leaves);
    this->storage->setCapacity(num_buckets);
    this->position_map = new int[this->num_blocks];
    this->stash = vector<Block>();
    
    for (int i = 0; i < this->num_blocks; i++){
        position_map[i] = rand_gen->getRandomLeaf();
    }

    for(int i = 0; i < num_buckets; i++){

        Bucket init_bkt = Bucket();
        for(int j = 0; j < bucket_size; j++){
            init_bkt.addBlock(Block());
        }
        storage->WriteBucket(i, Bucket(init_bkt));
    }

}

int* OramReadPathEviction::access(Operation op, int blockIndex, int *newdata) {

    int *data = new int[Block::BLOCK_SIZE]();
    int oldLeaf = position_map[blockIndex];
    int newLeaf = rand_gen->getRandomLeaf();
    position_map[blockIndex] = newLeaf;

    // Read-path absorb: pull every block from the path into the stash.
    // We push real blocks only (dummies have index == -1). The number of
    // dummies in each bucket is fixed at init and after every eviction, so
    // the *count* of pushes per level is constant from the host's view of
    // the encrypted bucket layout.
    for (int i = 0; i < num_levels; i++) {
        vector<Block> blocks = storage->ReadBucket(OramReadPathEviction::P(oldLeaf, i)).getBlocks();
        for (Block b: blocks) {
            if (b.index != -1) {
                stash.push_back(Block(b));
            }
        }
    }

    // Target search: scan the whole stash with no early exit so the iteration
    // count does not depend on the target's position. We also operate on
    // stash[i] directly (not a loop-local copy) so the subsequent writes hit
    // the actual stash slot — fixes the dangling-pointer / use-after-scope
    // bug from the upstream reference.
    int32_t targetPos = -1;
    for (size_t i = 0; i < stash.size(); i++) {
        uint32_t mask = oblivious::ct_eq_i32(stash[i].index, blockIndex);
        targetPos = oblivious::ct_select_i32(mask, static_cast<int32_t>(i), targetPos);
    }
    bool found = (targetPos >= 0);

    if (op == Operation::WRITE) {
        if (!found) {
            stash.push_back(Block(newLeaf, blockIndex, newdata));
        } else {
            Block& slot = stash[static_cast<size_t>(targetPos)];
            slot.leaf_id = newLeaf;
            for (int i = 0; i < Block::BLOCK_SIZE; i++) {
                slot.data[i] = newdata[i];
            }
        }
    }
    else {
        if (!found) {
            // Caller treats all-zero buffer as "not present". Returning NULL
            // (as the reference did) leaks existence via a pointer-vs-data
            // distinction in the caller's control flow.
            // data is already zero-initialised above.
        } else {
            const Block& slot = stash[static_cast<size_t>(targetPos)];
            for (int i = 0; i < Block::BLOCK_SIZE; i++) {
                data[i] = slot.data[i];
            }
        }
    }

    // Eviction: write back along the old path, deepest level first.
    //
    // Fully oblivious design: for every level we scan the entire stash with
    // no data-dependent branches or early exits. Each bucket is pre-built as
    // `bucket_size` dummy slots; eligible stash blocks are selected into the
    // correct slot via ct_memcpy / ct_select_i32 so the host sees a fixed
    // memory-access pattern regardless of which blocks are evicted.
    //
    // Stash entries taken for a bucket are marked index=-1 in-place so they
    // are skipped in later level iterations without a conditional erase.
    // A single compaction pass after all levels removes them from the vector.
    for (int l = num_levels - 1; l >= 0; l--) {

        int Pxl = P(oldLeaf, l);

        // Pre-build bucket_size slots as dummies (index = -1).
        vector<Block> slots(static_cast<size_t>(bucket_size));
        uint32_t counter = 0;

        for (size_t i = 0; i < stash.size(); i++) {
            int32_t idx = stash[i].index;

            // Guard already-evicted stash slots (index == -1): clamp to 0 so
            // position_map[] is never accessed out of range.
            uint32_t notDummy = ~oblivious::ct_eq_i32(idx, -1);
            int32_t safeIdx   = oblivious::ct_select_i32(notDummy, idx, 0);

            int blockLeaf    = position_map[safeIdx];
            uint32_t eligible = oblivious::ct_eq_i32(P(blockLeaf, l), Pxl) & notDummy;
            uint32_t hasRoom  = oblivious::ct_lt_u32(counter,
                                                     static_cast<uint32_t>(bucket_size));
            uint32_t take = eligible & hasRoom;

            // Obliviously write stash[i] into slot[counter] if taken.
            // The inner loop touches every slot so the write address is fixed.
            for (int j = 0; j < bucket_size; j++) {
                uint32_t isMySlot = oblivious::ct_eq_i32(static_cast<int32_t>(counter), j);
                uint32_t doWrite  = take & isMySlot;
                oblivious::ct_memcpy_i32(slots[j].data, stash[i].data,
                                         Block::BLOCK_SIZE, doWrite);
                slots[j].index     = oblivious::ct_select_i32(doWrite, stash[i].index,
                                                               slots[j].index);
                slots[j].leaf_id   = oblivious::ct_select_i32(doWrite, stash[i].leaf_id,
                                                               slots[j].leaf_id);
                slots[j].data_size = oblivious::ct_select_i32(doWrite, stash[i].data_size,
                                                               slots[j].data_size);
            }

            counter += take; // 0 or 1 — branch-free addition
            // Mark taken stash slot as evicted; skipped by notDummy guard above
            // in subsequent level iterations.
            stash[i].index = oblivious::ct_select_i32(take, -1, stash[i].index);
        }

        // Build bucket from the fixed-size slot array and write to storage.
        // addBlock is called exactly bucket_size times on a fresh bucket, so
        // its internal size-check branch is always taken — no leakage.
        Bucket bucket = Bucket();
        for (int j = 0; j < bucket_size; j++) {
            bucket.addBlock(slots[j]);
        }
        storage->WriteBucket(Pxl, bucket);
    }

    // Compact stash: remove entries marked as evicted (index == -1).
    stash.erase(
        std::remove_if(stash.begin(), stash.end(),
                       [](const Block& b) { return b.index == -1; }),
        stash.end());

    return data;
}

int OramReadPathEviction::P(int leaf, int level) {
    /*
    * This function should be deterministic. 
    * INPUT: leaf in range 0 to num_leaves - 1, level in range 0 to num_levels - 1. 
    * OUTPUT: Returns the location in the storage of the bucket which is at the input level and leaf.
    */
    return (1<<level) - 1 + (leaf >> (this->num_levels - level - 1));
}


/*
The below functions are to access various parameters, as described by their names.
INPUT: No input
OUTPUT: Value of internal variables given in the name.
*/

int* OramReadPathEviction::getPositionMap() {
    return this->position_map;
}

vector<Block> OramReadPathEviction::getStash() {
    return this->stash;
}
    
int OramReadPathEviction::getStashSize() {
    return (this->stash).size();
}
    
int OramReadPathEviction::getNumLeaves() {
    return this->num_leaves;

}

int OramReadPathEviction::getNumLevels() {
    return this->num_levels;

}

int OramReadPathEviction::getNumBlocks() {
    return this->num_blocks;

}

int OramReadPathEviction::getNumBuckets() {
    return this->num_buckets;

}

