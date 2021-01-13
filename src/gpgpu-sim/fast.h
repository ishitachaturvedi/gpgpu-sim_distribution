#include<vector>
using std::vector;

extern int cycle_num;
//initialise vector of vectors as stallData[Warp #][stall #]
extern vector<vector<int>>stallData;
extern vector<int>act_warp;
//types of stalls
extern int numstall;
extern int idlew;
extern int ocfull; //no OC to allocate
extern int ocempty; //no OC to dispatch
extern int mem_str;
extern int mem_data;
extern int synco;
extern int comp_str;
extern int comp_data;
extern int control;
extern int ibufferw;
extern int imisspendingw;
extern int pendingWritew;
//max number of warps active
extern int max_active;
extern int max_warps_act;

