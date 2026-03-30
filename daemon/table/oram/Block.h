//
//
//

#ifndef PORAM_BLOCK_H
#define PORAM_BLOCK_H

#include <algorithm>
using namespace std;

class Block {
public:
    static const int BLOCK_SIZE = 2200; // 8800 bytes - tamanho máximo de um Data Packet Ndn
    int leaf_id;
    int index;
    int data[BLOCK_SIZE];
    int data_size;

    Block();
    Block(int leaf_id, int index, int data[], int data_size = 0);
    void printBlock();
    virtual ~Block();
};

#endif //PORAM_BLOCK_H
