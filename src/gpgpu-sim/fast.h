#include<vector>
using std::vector;

extern int cycle_num;
//initialise vector of vectors as stallData[Warp #][stall #]
extern vector<vector<vector<int>>>stallData;
extern vector<int>act_warp;

//types of stalls
extern int numstall;
typedef enum {
    mem_str = 0,
    mem_data,
    synco,
    comp_str,
    comp_data,
    control,
    ibufferw,
    imisspendingw,
    pendingWritew,
    idlew
} StallReasons;

//max number of warps active
extern int max_active;
extern int max_warps_act;
extern int max_oc_avail;
extern int oc_alloc;
extern int max_oc_disp;
extern int oc_disp;
extern int max_sid;

extern vector<int> nDispatch;
extern vector<int> warpDispatch;