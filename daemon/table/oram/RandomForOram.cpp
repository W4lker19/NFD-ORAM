//
//
//

#include "RandomForOram.h"

int RandomForOram::getRandomLeaf()
{
    long next = m_rng();
    int val = static_cast<int>(next % m_bound);
    if (val < 0) {
        val += m_bound;
    }
    return val;
}

void RandomForOram::setBound(int totalNumOfLeaves)
{
    m_bound = totalNumOfLeaves;
}
