//
//
//
#include "Block.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cstring>

using namespace std;

Block::Block(){//dummy index
    this->leaf_id = -1;
    this->index = -1;
    this->data_size = 0;
    memset(this->data, 0 , sizeof(this->data));
}

Block::Block(int leaf_id, int index, int data[], int data_size)
    : leaf_id(leaf_id), index(index), data_size(data_size)
{
    memset(this->data, 0, sizeof(this->data));
    int copySize = (data_size > 0) ? data_size : BLOCK_SIZE;
    for (int i = 0; i < copySize && i < BLOCK_SIZE; i++)
    {
        this->data[i] = data[i];
    }
}

Block::~Block(){}

void Block::printBlock()
{
    cout << "index: " << to_string(this->index)
         << " leaf id: " << to_string(this->leaf_id)
         << " data_size: " << to_string(this->data_size)
         << " data[0]: " << to_string(this->data[0])
         << " data[1]: " << to_string(this->data[1])
         << endl;
}