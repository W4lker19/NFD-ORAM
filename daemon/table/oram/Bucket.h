//
//
//

#ifndef PORAM_BUCKET_H
#define PORAM_BUCKET_H


#include "Block.h"
#include <vector>
#include <stdexcept>

class Bucket {

public:
    Bucket();
    Bucket(Bucket *other);
    Block getBlockByIndex(int index);
    void addBlock(Block new_blk);
    bool removeBlock(Block rm_blk);
    vector<Block> getBlocks();
    void setMaxSize(int maximumSize);
    void resetState();
    int getMaxSize();
    void printBlocks();

private:
    bool is_init = false;
    int max_size = 4;
    vector<Block> blocks;
};


#endif //PORAM_BUCKET_H
