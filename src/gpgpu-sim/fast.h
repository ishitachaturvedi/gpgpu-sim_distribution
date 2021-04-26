#include<vector>
using std::vector;

extern int cycle_num;
//initialise vector of vectors as stallData[Warp #][stall #]
extern vector<vector<vector<int>>>stallData;
extern vector<vector<int>>act_warp;
extern vector<vector<vector<int>>> str_status;

//types of stalls
extern int numstall;

//max number of warps active
extern int max_active;
extern int max_warps_act;
extern int max_oc_avail;
extern int oc_alloc;
extern int max_oc_disp;
extern int oc_disp;
extern int max_sid;
extern int num_of_schedulers;
extern int cycle_counter;

extern int actw;

extern vector<vector<int>> nDispatch;
extern vector<vector<int>> warpDispatch;
