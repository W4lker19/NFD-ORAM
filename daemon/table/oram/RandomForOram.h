//
//
//

#ifndef PORAM_RANDOMFORORAM_H
#define PORAM_RANDOMFORORAM_H
#include <cmath>
#include <stdlib.h>
#include <vector>
#include <random>
#include "RandForOramInterface.h"
#include "duthomhas/csprng.hpp"

using namespace std;

class RandomForOram : public RandForOramInterface {
	public:
		bool is_initialized = false;
		int bound = -1;
		vector<int> rand_history;
		RandomForOram();
		void RandomForOramMT();
		void RandomForOramLCG();
		int getRandomLeaf();
		int getRandomLeafMT();
		int getRandomLeafLCG();
		void setBound(int totalNumOfLeaves);
		void resetState();
		void clearHistory();
		vector<int> getHistory();
		linear_congruential_engine<unsigned long, 25214903917, 11, 281474976710656> rnd_generator;
		std::mt19937 mt_generator; 
		long seed = 0L;
		duthomhas::csprng rng;
};


//static int RandomForOram::bound = -1; 


#endif 
