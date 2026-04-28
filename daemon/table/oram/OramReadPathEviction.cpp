//
//
//

#include "OramReadPathEviction.h"
#include "ObliviousOps.h"
#include <cstdint>
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

    // Eviction: write back along the old path, deepest level first. We scan
    // the whole stash at every level (no early exit on bucket-full) so the
    // per-level work is independent of which specific stash slots are eligible.
    // The actual addBlock / erase calls remain conditional; making them fully
    // oblivious requires a fixed-size per-slot cmov fill, which is left for
    // a follow-up refactor (see TEE hardening notes).
    for (int l = num_levels - 1; l >= 0; l--) {

        Bucket bucket = Bucket();
        int Pxl = P(oldLeaf, l);
        int counter = 0;
        vector<size_t> evicted_positions;
        evicted_positions.reserve(static_cast<size_t>(bucket_size));

        for (size_t i = 0; i < stash.size(); i++) {
            const Block& b = stash[i];
            // Always read position_map[b.index] (no branch on b.index validity);
            // we already filtered dummies on absorb so b.index is in range.
            int blockLeaf = position_map[b.index];
            int Pblock = P(blockLeaf, l);
            uint32_t eligible = oblivious::ct_eq_i32(Pblock, Pxl);
            uint32_t hasRoom  = oblivious::ct_lt_u32(static_cast<uint32_t>(counter),
                                                    static_cast<uint32_t>(bucket_size));
            if ((eligible & hasRoom) != 0u) {
                bucket.addBlock(b);
                evicted_positions.push_back(i);
                counter++;
            }
        }

        // Remove evicted entries from the stash. Iterate from the back so
        // earlier indices stay valid.
        for (size_t k = evicted_positions.size(); k-- > 0;) {
            stash.erase(stash.begin() + evicted_positions[k]);
        }

        while (counter < bucket_size) {
            bucket.addBlock(Block()); // dummy
            counter++;
        }
        storage->WriteBucket(Pxl, bucket);
    }

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

