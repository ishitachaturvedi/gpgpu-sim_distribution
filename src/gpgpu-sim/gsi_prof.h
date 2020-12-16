//Defining all variables required by GSI here
//initialisation is done in src/gpgpusim_entrypoint.cc
//GPU_stuff
#include<vector>
using std::vector;

//vectors to store the stall type for each warp
//0-memory structure stall
//1-memory data stall
//2-synchronization stall
//3-compute structural stall
//4-compute data stall
//5-control stall
//6-idle stall

//give initial size for number of cycles to be considered
extern int initSize;
//initialise vector of vectors as stallData[Cycle #][Warp #]
extern vector<int> stallData;
//define temp array to store data
extern vector<int> tempw;
//if warp active corresponding value set to 1
extern vector<int> activew;
extern int active_warp;
extern int cycle_num;
//give priority to different stalls
//0-max priority
//10-least priority
extern int mem_str;
extern int mem_data;
extern int synco;
extern int comp_str;
extern int comp_data;
extern int control;
extern int idle;
extern int idlew;
