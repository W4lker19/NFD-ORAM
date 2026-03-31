//
//
//

#ifndef PORAM_SERVERSTORAGE_H
#define PORAM_SERVERSTORAGE_H
#include "OramInterface.h"
#include "RandForOramInterface.h"
#include "UntrustedStorageInterface.h"
#include <cmath>

class ServerStorage : public UntrustedStorageInterface {
public:
    
    bool is_initialized;
    bool is_capacity_set;
    //Bucket* buckets;
    std::vector<Bucket> buckets;
    
    ServerStorage();
    void setCapacity(int totalNumOfBuckets);
    Bucket ReadBucket(int position);
    void WriteBucket(int position, const Bucket& bucket_to_write);

private: 

    int capacity;

};


#endif //PORAM_ORAMREADPATHEVICTION_H
