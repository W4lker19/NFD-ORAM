//
//
//

#ifndef PORAM_RANDOMFORORAM_H
#define PORAM_RANDOMFORORAM_H

#include "RandForOramInterface.h"
#include "duthomhas/csprng.hpp"

class RandomForOram : public RandForOramInterface {
public:
    RandomForOram() = default;
    int getRandomLeaf() override;
    void setBound(int totalNumOfLeaves) override;

private:
    int m_bound = -1;
    duthomhas::csprng m_rng;
};

#endif
