//
//
//
#include "Bucket.h"
#include "ObliviousOps.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <sstream>

using namespace std;

bool Bucket::is_init = false;
int Bucket::max_size = -1;

Bucket::Bucket(){
    if(!is_init){
        throw runtime_error("Please set bucket size before creating a bucket");
    }
    blocks = vector<Block>();
}

//Copy constructor
Bucket::Bucket(Bucket *other){
    if(other == NULL){
        throw runtime_error("the other bucket is not malloced.");
    }
    blocks = vector<Block>(max_size);
    for(int i = 0; i < max_size; i++){
        blocks[i] = Block(other->blocks[i]);
    }
}

// Get block object with matching index. Returns a dummy Block (index == -1)
// when no match is found — the original implementation dereferenced a NULL
// pointer in that case. Scans the entire bucket with no early exit so the
// access pattern does not depend on which slot held the match.
Block Bucket::getBlockByIndex(int index) {
    Block result; // dummy: index = -1
    for (size_t i = 0; i < blocks.size(); i++) {
        uint32_t mask = oblivious::ct_eq_i32(blocks[i].index, index);
        // Conditionally copy the block fields. data[] is the bulk; leaf/index
        // are scalars also copied conditionally.
        oblivious::ct_memcpy(result.data, blocks[i].data,
                             sizeof(result.data), mask);
        result.index   = oblivious::ct_select_i32(mask, blocks[i].index,   result.index);
        result.leaf_id = oblivious::ct_select_i32(mask, blocks[i].leaf_id, result.leaf_id);
        result.data_size = oblivious::ct_select_i32(mask, blocks[i].data_size, result.data_size);
    }
    return result;
}

void Bucket::addBlock(Block new_blk){
    if(blocks.size() < static_cast<size_t>(max_size)){
        Block toAdd = Block(new_blk);
        blocks.push_back(toAdd);
    }

}

// Removes the first block whose index matches rm_blk.index.
// Scans the entire bucket; the actual erase still branches on the match
// position because std::vector::erase is not constant-time. Making this
// fully oblivious requires a fixed-layout backing store (e.g. a plain
// array with per-slot valid bits) and is left for the TEE follow-up.
bool Bucket::removeBlock(Block rm_blk){
    int32_t hitPos = -1;
    for (size_t i = 0; i < blocks.size(); i++) {
        uint32_t mask = oblivious::ct_eq_i32(blocks[i].index, rm_blk.index);
        // Keep the first match: only update hitPos if we don't have one yet.
        uint32_t empty = oblivious::ct_eq_i32(hitPos, -1);
        hitPos = oblivious::ct_select_i32(mask & empty,
                                          static_cast<int32_t>(i), hitPos);
    }
    if (hitPos >= 0) {
        blocks.erase(blocks.begin() + static_cast<size_t>(hitPos));
        return true;
    }
    return false;
}

// Return a shallow copy.
vector<Block> Bucket::getBlocks(){
    return this->blocks;
}

void Bucket::setMaxSize(int maximumSize){
    if(is_init == true){
        throw runtime_error("Max Bucket Size was already set");
    }
    max_size = maximumSize;
    is_init = true;
}

int Bucket::getMaxSize() {
    return max_size;
}

void Bucket::resetState(){
    is_init = false;
}

void Bucket::printBlocks() {
    for (Block b: blocks) {
        b.printBlock();
    }
}
