#STALL ORDERING:
#mem_str = 0,
#mem_data,
#synco,
#comp_str,
#comp_data,
#control,
#ibufferw,
#imisspendingw,
#pendingWritew,
#idlew

#IDEA: Each SM and each sched of each SM is independent--> speedup on removing a stall type is the minimum speedup among all SMs when the stall is removed

from copy import deepcopy
from pathlib import Path

#sort function
def Sort(sub_li,key): 
    sub_li.sort(key = lambda x: x[key],reverse=False) 
    return sub_li 

#initialise data structures to keep count of stalls
def init_one(num_shaders,num_sched,numStalls):
    warp_indep_c = []
    for shader in range(num_shaders):
        warp_indep_shader = []
        for sched in range(num_sched):
            warp_indep_sched = []
            for i in range(numStalls):
                    warp_indep_sched.append(0)
            warp_indep_shader.append(warp_indep_sched)
        warp_indep_c.append(warp_indep_shader)
    return warp_indep_c

def init_two(num_shaders,num_sched,numStalls):
    warp_two_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall1.append(0)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_two_c.append(warp_shader)
    return warp_two_c

def init_three(num_shaders,num_sched,numStalls):
    warp_three_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall2.append(0)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_three_c.append(warp_shader)
    return warp_three_c

def init_four(num_shaders,num_sched,numStalls):
    warp_four_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall3.append(0)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_four_c.append(warp_shader)
    return warp_four_c

def init_five(num_shaders,num_sched,numStalls):
    warp_five_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall4 = []
                            for k2 in range(k1+1,numStalls):
                                stall4.append(0)
                            stall3.append(stall4)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_five_c.append(warp_shader)
    return warp_five_c

def init_six(num_shaders,num_sched,numStalls):
    warp_six_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall4 = []
                            for k2 in range(k1+1,numStalls):
                                stall5 = []
                                for k3 in range(k2+1,numStalls):
                                    stall5.append(0)
                                stall4.append(stall5)
                            stall3.append(stall4)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_six_c.append(warp_shader)
    return warp_six_c

def init_seven(num_shaders,num_sched,numStalls):
    warp_seven_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall4 = []
                            for k2 in range(k1+1,numStalls):
                                stall5 = []
                                for k3 in range(k2+1,numStalls):
                                    stall6 = []
                                    for k4 in range(k3+1,numStalls):
                                        stall6.append(0)
                                    stall5.append(stall6)
                                stall4.append(stall5)
                            stall3.append(stall4)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_seven_c.append(warp_shader)
    return warp_seven_c

def init_eight(num_shaders,num_sched,numStalls):
    warp_eight_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall4 = []
                            for k2 in range(k1+1,numStalls):
                                stall5 = []
                                for k3 in range(k2+1,numStalls):
                                    stall6 = []
                                    for k4 in range(k3+1,numStalls):
                                        stall7 = []
                                        for k5 in range(k4+1,numStalls):
                                            stall7.append(0)
                                        stall6.append(stall7)
                                    stall5.append(stall6)
                                stall4.append(stall5)
                            stall3.append(stall4)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_eight_c.append(warp_shader)
    return warp_eight_c

def init_nine(num_shaders,num_sched,numStalls):
    warp_nine_c = []
    for shader in range(num_shaders):
        warp_shader = []
        for sched in range(num_sched):
            warp_sched = []
            for  i in range(numStalls):
                stall1 = []
                for j in range(i+1,numStalls):
                    stall2 = []
                    for k in range(j+1,numStalls):
                        stall3 = []
                        for k1 in range(k+1,numStalls):
                            stall4 = []
                            for k2 in range(k1+1,numStalls):
                                stall5 = []
                                for k3 in range(k2+1,numStalls):
                                    stall6 = []
                                    for k4 in range(k3+1,numStalls):
                                        stall7 = []
                                        for k5 in range(k4+1,numStalls):
                                            stall8 = []
                                            for k6 in range(k5+1,numStalls):
                                                stall8.append(0)
                                            stall7.append(stall8)
                                        stall6.append(stall7)
                                    stall5.append(stall6)
                                stall4.append(stall5)
                            stall3.append(stall4)
                        stall2.append(stall3)
                    stall1.append(stall2)
                warp_sched.append(stall1)
            warp_shader.append(warp_sched)
        warp_nine_c.append(warp_shader)
    return warp_nine_c

# Functions to initialise Data structures which store the final result
def init_one_final(numStalls):
    warp_indep_c = []
    # Data_Structure - warp_indep_c[stall][#of cycles after stall removal, #cycles before stall removal]
    for i in range(numStalls):
        temp = []
        temp1 = []
        temp.append(i)
        temp1.append(0)
        temp1.append(0)
        temp.append(temp1)
        warp_indep_c.append(temp)
    return warp_indep_c

def init_two_final(numStalls):
    warp_two_c = []
    # Data_Structure - warp_two_c[stall1][stall2][#of cycles after stall removal, #cycles before stall removal]
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            temp1 = []
            temp1.append(0)
            temp1.append(0)
            stall1.append(temp1)
        warp_two_c.append(stall1)
    return warp_two_c

def init_three_final(numStalls):
    warp_three_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                temp1=[]
                temp1.append(0)
                temp1.append(0)
                stall2.append(temp1)
            stall1.append(stall2)
        warp_three_c.append(stall1)
    return warp_three_c

def init_four_final(numStalls):
    warp_four_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    temp1=[]
                    temp1.append(0)
                    temp1.append(0)
                    stall3.append(temp1)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_four_c.append(stall1)
    return warp_four_c

def init_five_final(numStalls):
    warp_five_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    stall4 = []
                    for k2 in range(k1+1,numStalls):
                        temp1=[]
                        temp1.append(0)
                        temp1.append(0)
                        stall4.append(temp1)
                    stall3.append(stall4)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_five_c.append(stall1)
    return warp_five_c

def init_six_final(numStalls):
    warp_six_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    stall4 = []
                    for k2 in range(k1+1,numStalls):
                        stall5 = []
                        for k3 in range(k2+1,numStalls):
                            temp1=[]
                            temp1.append(0)
                            temp1.append(0)
                            stall5.append(temp1)
                        stall4.append(stall5)
                    stall3.append(stall4)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_six_c.append(stall1)
    return warp_six_c

def init_seven_final(numStalls):
    warp_seven_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    stall4 = []
                    for k2 in range(k1+1,numStalls):
                        stall5 = []
                        for k3 in range(k2+1,numStalls):
                            stall6 = []
                            for k4 in range(k3+1,numStalls):
                                temp1=[]
                                temp1.append(0)
                                temp1.append(0)
                                stall6.append(temp1)
                            stall5.append(stall6)
                        stall4.append(stall5)
                    stall3.append(stall4)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_seven_c.append(stall1)
    return warp_seven_c

def init_eight_final(numStalls):
    warp_eight_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    stall4 = []
                    for k2 in range(k1+1,numStalls):
                        stall5 = []
                        for k3 in range(k2+1,numStalls):
                            stall6 = []
                            for k4 in range(k3+1,numStalls):
                                stall7 = []
                                for k5 in range(k4+1,numStalls):
                                    temp1=[]
                                    temp1.append(0)
                                    temp1.append(0)
                                    stall7.append(temp1)
                                stall6.append(stall7)
                            stall5.append(stall6)
                        stall4.append(stall5)
                    stall3.append(stall4)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_eight_c.append(stall1) 
    return warp_eight_c

def init_nine_final(numStalls):
    warp_nine_c = []
    for i in range(numStalls):
        stall1 = []
        for j in range(i+1,numStalls):
            stall2 = []
            for k in range(j+1,numStalls):
                stall3 = []
                for k1 in range(k+1,numStalls):
                    stall4 = []
                    for k2 in range(k1+1,numStalls):
                        stall5 = []
                        for k3 in range(k2+1,numStalls):
                            stall6 = []
                            for k4 in range(k3+1,numStalls):
                                stall7 = []
                                for k5 in range(k4+1,numStalls):
                                    stall8 = []
                                    for k6 in range(k5+1,numStalls):
                                        temp1=[]
                                        temp1.append(0)
                                        temp1.append(0)
                                        stall8.append(temp1)
                                    stall7.append(stall8)
                                stall6.append(stall7)
                            stall5.append(stall6)
                        stall4.append(stall5)
                    stall3.append(stall4)
                stall2.append(stall3)
            stall1.append(stall2)
        warp_nine_c.append(stall1)
    return warp_nine_c

# Functions to find the min speedup among all scheds and SMs and put them as final answers for various stall combinations
def indeps(numStalls,warp_indep_c,counter,num_shaders,num_sched):

    # List to store final twostall sorted values
    # List of lists with dataStructure [[stall1,#reduced cyces,#total cycles],[stall1,#reduced cyces,#total cycles]..]
    final_stall = []

    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall in range(numStalls):
        # Max number of cycles left after removing the stall
        maxCyclesLeft = 0
        # Cycles the longest running SM after stall removal actually had run for 
        cyclesSMRan = 0
        for SM in range(num_shaders):
            for sched in range(num_sched):
                diff = counter[SM][sched] - warp_indep_c[SM][sched][stall]
                if (diff >  maxCyclesLeft):
                    maxCyclesLeft = diff
                    cyclesSMRan = counter[SM][sched]
        # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
        temp = []
        temp.append(stall)
        temp.append(maxCyclesLeft)
        temp.append(cyclesSMRan)
        final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,1)
    return final_stall

def twos(numStalls,warp_two_c,counter,num_shaders,num_sched):
    
    # List to store final twostall sorted values
    # List of lists with dataStructure [[stall1,stall2,#reduced cyces,#total cycles],[stall1,stall2,#reduced cyces,#total cycles]..]
    final_stall = [] 
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            # Max number of cycles left after removing the stall
            maxCyclesLeft = 0
            # Cycles the longest running SM after stall removal actually had run for 
            cyclesSMRan = 0
            for SM in range(num_shaders):
                for sched in range(num_sched):
                    diff = counter[SM][sched] - warp_two_c[SM][sched][stall1][stall2]
                    if (diff >  maxCyclesLeft):
                        maxCyclesLeft = diff
                        cyclesSMRan = counter[SM][sched]
            # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
            temp = []
            temp.append(stall1)
            temp.append(stall2i)
            temp.append(maxCyclesLeft)
            temp.append(cyclesSMRan)
            final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,2)
    return final_stall

def threes(numStalls,warp_three_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                # Max number of cycles left after removing the stall
                maxCyclesLeft = 0
                # Cycles the longest running SM after stall removal actually had run for 
                cyclesSMRan = 0
                for SM in range(num_shaders):
                    for sched in range(num_sched):
                        diff = counter[SM][sched] - warp_three_c[SM][sched][stall1][stall2][stall3]
                        if (diff >  maxCyclesLeft):
                            maxCyclesLeft = diff
                            cyclesSMRan = counter[SM][sched]
                # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                temp = []
                temp.append(stall1)
                temp.append(stall2i)
                temp.append(stall3i)
                temp.append(maxCyclesLeft)
                temp.append(cyclesSMRan)
                final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,3)
    return final_stall

def fours(numStalls,warp_four_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    # Max number of cycles left after removing the stall
                    maxCyclesLeft = 0
                    # Cycles the longest running SM after stall removal actually had run for 
                    cyclesSMRan = 0
                    for SM in range(num_shaders):
                        for sched in range(num_sched):
                            diff = counter[SM][sched] - warp_four_c[SM][sched][stall1][stall2][stall3][stall4]
                            if (diff >  maxCyclesLeft):
                                maxCyclesLeft = diff
                                cyclesSMRan = counter[SM][sched]
                    # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                    temp = []
                    temp.append(stall1)
                    temp.append(stall2i)
                    temp.append(stall3i)
                    temp.append(stall4i)
                    temp.append(maxCyclesLeft)
                    temp.append(cyclesSMRan)
                    final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,4)
    return final_stall

def fives(numStalls,warp_five_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    for stall5i in range(stall4i+1,numStalls):
                        stall5 = stall5i - (stall4i+1)
                        # Max number of cycles left after removing the stall
                        maxCyclesLeft = 0
                        # Cycles the longest running SM after stall removal actually had run for 
                        cyclesSMRan = 0
                        for SM in range(num_shaders):
                            for sched in range(num_sched):
                                diff = counter[SM][sched] - warp_five_c[SM][sched][stall1][stall2][stall3][stall4][stall5]
                                if (diff >  maxCyclesLeft):
                                    maxCyclesLeft = diff
                                    cyclesSMRan = counter[SM][sched]
                        # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                        temp = []
                        temp.append(stall1)
                        temp.append(stall2i)
                        temp.append(stall3i)
                        temp.append(stall4i)
                        temp.append(stall5i)
                        temp.append(maxCyclesLeft)
                        temp.append(cyclesSMRan)
                        final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,5)
    return final_stall

def sixs(numStalls,warp_six_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    for stall5i in range(stall4i+1,numStalls):
                        stall5 = stall5i - (stall4i+1)
                        for stall6i in range(stall5i+1,numStalls):
                            stall6 = stall6i - (stall5i+1)
                            # Max number of cycles left after removing the stall
                            maxCyclesLeft = 0
                            # Cycles the longest running SM after stall removal actually had run for 
                            cyclesSMRan = 0
                            for SM in range(num_shaders):
                                for sched in range(num_sched):
                                    diff = counter[SM][sched] - warp_six_c[SM][sched][stall1][stall2][stall3][stall4][stall5][stall6]
                                    if (diff >  maxCyclesLeft):
                                        maxCyclesLeft = diff
                                        cyclesSMRan = counter[SM][sched]
                            # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                            temp = []
                            temp.append(stall1)
                            temp.append(stall2i)
                            temp.append(stall3i)
                            temp.append(stall4i)
                            temp.append(stall5i)
                            temp.append(stall6i)
                            temp.append(maxCyclesLeft)
                            temp.append(cyclesSMRan)
                            final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,6)
    return final_stall

def sevens(numStalls,warp_seven_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []

    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    for stall5i in range(stall4i+1,numStalls):
                        stall5 = stall5i - (stall4i+1)
                        for stall6i in range(stall5i+1,numStalls):
                            stall6 = stall6i - (stall5i+1)
                            for stall7i in range(stall6i+1,numStalls):
                                stall7 = stall7i - (stall6i+1)
                                # Max number of cycles left after removing the stall
                                maxCyclesLeft = 0
                                # Cycles the longest running SM after stall removal actually had run for 
                                cyclesSMRan = 0
                                for SM in range(num_shaders):
                                    for sched in range(num_sched):
                                        diff = counter[SM][sched] - warp_seven_c[SM][sched][stall1][stall2][stall3][stall4][stall5][stall6][stall7]
                                        if (diff >  maxCyclesLeft):
                                            maxCyclesLeft = diff
                                            cyclesSMRan = counter[SM][sched]
                                # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                                temp = []
                                temp.append(stall1)
                                temp.append(stall2i)
                                temp.append(stall3i)
                                temp.append(stall4i)
                                temp.append(stall5i)
                                temp.append(stall6i)
                                temp.append(stall7i)
                                temp.append(maxCyclesLeft)
                                temp.append(cyclesSMRan)
                                final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,7)
    return final_stall

def eights(numStalls,warp_eight_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []
    
    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    for stall5i in range(stall4i+1,numStalls):
                        stall5 = stall5i - (stall4i+1)
                        for stall6i in range(stall5i+1,numStalls):
                            stall6 = stall6i - (stall5i+1)
                            for stall7i in range(stall6i+1,numStalls):
                                stall7 = stall7i - (stall6i+1)
                                for stall8i in range(stall7i+1,numStalls):
                                    stall8 = stall8i - (stall7i+1)
                                    # Max number of cycles left after removing the stall
                                    maxCyclesLeft = 0
                                    # Cycles the longest running SM after stall removal actually had run for 
                                    cyclesSMRan = 0
                                    for SM in range(num_shaders):
                                        for sched in range(num_sched):
                                            diff = counter[SM][sched] - warp_eight_c[SM][sched][stall1][stall2][stall3][stall4][stall5][stall6][stall7][stall8]
                                            if (diff >  maxCyclesLeft):
                                                maxCyclesLeft = diff
                                                cyclesSMRan = counter[SM][sched]
                                    # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                                    temp = []
                                    temp.append(stall1)
                                    temp.append(stall2i)
                                    temp.append(stall3i)
                                    temp.append(stall4i)
                                    temp.append(stall5i)
                                    temp.append(stall6i)
                                    temp.append(stall7i)
                                    temp.append(stall8i)
                                    temp.append(maxCyclesLeft)
                                    temp.append(cyclesSMRan)
                                    final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,8)
    return final_stall

def nines(numStalls,warp_nine_c,counter,num_shaders,num_sched):
    # List to store final twostall sorted values
    final_stall = []

    # For each stall find the minimum speedup across all SMs -  min speedup = max # of cycles left after (total cycles the sched in SM runs for - #cycles stalled because of a particular stall)
    for stall1 in range(numStalls):
        for stall2i in range(stall1+1,numStalls):
            stall2 = stall2i - (stall1+1)
            for stall3i in range(stall2i+1,numStalls):
                stall3 = stall3i - (stall2i+1)
                for stall4i in range(stall3i+1,numStalls):
                    stall4 = stall4i - (stall3i+1)
                    for stall5i in range(stall4i+1,numStalls):
                        stall5 = stall5i - (stall4i+1)
                        for stall6i in range(stall5i+1,numStalls):
                            stall6 = stall6i - (stall5i+1)
                            for stall7i in range(stall6i+1,numStalls):
                                stall7 = stall7i - (stall6i+1)
                                for stall8i in range(stall7i+1,numStalls):
                                    stall8 = stall8i - (stall7i+1)
                                    for stall9i in range(stall8i+1,numStalls):
                                        stall9 = stall9i - (stall8i+1)
                                        # Max number of cycles left after removing the stall
                                        maxCyclesLeft = 0
                                        # Cycles the longest running SM after stall removal actually had run for 
                                        cyclesSMRan = 0
                                        for SM in range(num_shaders):
                                            for sched in range(num_sched):
                                                diff = counter[SM][sched] - warp_nine_c[SM][sched][stall1][stall2][stall3][stall4][stall5][stall6][stall7][stall8][stall9]
                                                if (diff >  maxCyclesLeft):
                                                    maxCyclesLeft = diff
                                                    cyclesSMRan = counter[SM][sched]
                                        # Now add the # cycles left after stall removal and total number of cycles the SM ran for with stalls to indep
                                        temp = []
                                        temp.append(stall1)
                                        temp.append(stall2i)
                                        temp.append(stall3i)
                                        temp.append(stall4i)
                                        temp.append(stall5i)
                                        temp.append(stall6i)
                                        temp.append(stall7i)
                                        temp.append(stall8i)
                                        temp.append(stall9i)
                                        temp.append(maxCyclesLeft)
                                        temp.append(cyclesSMRan)
                                        final_stall.append(temp)

    # All the required values are here, now sort them in ascending order
    final_stall = Sort(final_stall,9)
    return final_stall

# For each shader and sched initialise a counter
def intitialiseCounter(num_shaders,num_sched):
    counter = []
    for shader in range(num_shaders):
        warp_shader =[]
        for sched in range(num_sched):
            warp_shader.append(0)
        counter.append(warp_shader)
    return counter

# Assign warps to the right schedular and make a list against which all incoming warps will be compared to check which sched they belong to
def assignSchedWarps(max_warps,num_sched):
    sched0 = []
    sched1 = []
    sched2 = []
    sched3 = []
    for warp in range(max_warps):
        if(warp % num_sched == 0):
            sched0.append(warp)
        elif(warp % num_sched == 1):
            sched1.append(warp)
        elif(warp % num_sched == 2):
            sched2.append(warp)
        elif(warp % num_sched == 3):
            sched3.append(warp)
    return sched0, sched1, sched2, sched3

# Function to add stalls to indep list
def indepStallAdd(warp_indep_c,StallList,SM,sched_num):
    id0 = StallList[0]
    warp_indep_c[SM][sched_num][id0] = warp_indep_c[SM][sched_num][id0] + 1
    return warp_indep_c

# Function to add stalls to two stall list
def TwoStallAdd(warp_two_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    warp_two_c[SM][sched_num][id0][id1-id0-1] = warp_two_c[SM][sched_num][id0][id1-id0-1] + 1
    return warp_two_c

# Function to add stalls to three stall list
def ThreeStallAdd(warp_three_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    warp_three_c[SM][sched_num][id0][id1-id0-1][id2-id1-1] = warp_three_c[SM][sched_num][id0][id1-id0-1][id2-id1-1] + 1
    return warp_three_c

# Function to add stalls to four stall list
def FourStallAdd(warp_four_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    warp_four_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1] = warp_four_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1] + 1
    return warp_four_c

# Function to add stalls to five stall list
def FiveStallAdd(warp_five_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    id4 = StallList[4]
    warp_five_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1] = warp_five_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1] + 1
    return warp_five_c

# Function to add stalls to six stall list
def SixStallAdd(warp_six_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    id4 = StallList[4]
    id5 = StallList[5]
    warp_six_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1] = warp_six_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1] + 1
    return warp_six_c

# Function to add stalls to seven stall list
def SevenStallAdd(warp_seven_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    id4 = StallList[4]
    id5 = StallList[5]
    id6 = StallList[6]
    warp_seven_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1] = warp_seven_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1] + 1
    return warp_seven_c

# Function to add stalls to eight stall list
def EightStallAdd(warp_eight_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    id4 = StallList[4]
    id5 = StallList[5]
    id6 = StallList[6]
    id7 = StallList[7]
    warp_eight_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1][id7-id6-1] = warp_eight_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1][id7-id6-1] + 1
    return warp_eight_c

# Function to add stalls to nine stall list
def NineStallAdd(warp_nine_c,StallList,SM,sched_num):
    id0 = StallList[0]
    id1 = StallList[1]
    id2 = StallList[2]
    id3 = StallList[3]
    id4 = StallList[4]
    id5 = StallList[5]
    id6 = StallList[6]
    id7 = StallList[7]
    id8 = StallList[8]
    warp_nine_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1][id7-id6-1][id8-id7-1] = warp_nine_c[SM][sched_num][id0][id1-id0-1][id2-id1-1][id3-id2-1][id4-id3-1][id5-id4-1][id6-id5-1][id7-id6-1][id8-id7-1] + 1
    return warp_nine_c

# Function to increase the right stall count based on the stalls in a scheduler per cycle
def putStallsInRightArray(SchedStallKeeper,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,sched_num):
    # Based on the stall combination, add it to the correct list
    for StallList in SchedStallKeeper:
        stallSize = len(StallList)
        #Based on the stallSize write to the correct stallList (indep, two, three ...)
        if (stallSize == 1):
            warp_indep_c = indepStallAdd(warp_indep_c,StallList,SM,sched_num)
        if (stallSize == 2):
            warp_two_c = TwoStallAdd(warp_two_c,StallList,SM,sched_num)
        if (stallSize == 3):
            warp_three_c = ThreeStallAdd(warp_three_c,StallList,SM,sched_num)
        if (stallSize == 4):
            warp_four_c = FourStallAdd(warp_four_c,StallList,SM,sched_num)
        if (stallSize == 5):
            warp_five_c = FiveStallAdd(warp_five_c,StallList,SM,sched_num)
        if (stallSize == 6):
            warp_six_c = SixStallAdd(warp_six_c,StallList,SM,sched_num)
        if (stallSize == 7):
            warp_seven_c = SevenStallAdd(warp_seven_c,StallList,SM,sched_num)
        if (stallSize == 8):
            warp_eight_c = EightStallAdd(warp_eight_c,StallList,SM,sched_num)
        if (stallSize == 9):
            warp_nine_c = NineStallAdd(warp_nine_c,StallList,SM,sched_num)

    return warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c
        

# Function to parse the data in the outfile per scheduler to assign stalls
def assignStalls(sched,keepSMData,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,sched_num):
    # Make note if all warps in a scheduler are idle in which case the counter for the schedular for the SM is not increased, since this cycle was because of warps in other schedulers and 
    # absence of this cycle for this sched would make no difference for the warps it has
    idle = 1
    # Data structure to store unique stalls in a warp, if a particular combination of stall was present in a previous warp, it will not be added, to account for a particular kind of stall ONCE per cycle
    SchedStallKeeper = []
    for line in keepSMData:
        if(line[0] == 'warp'):
            # check if the warp being looked into belongs to this scheduler
            # This dataStructure keeps the number of stalls and their location. Its size is the number of stalls, and each value inside it is the stall number
            WarpStallKeeper = []
            #if (line[1] in sched):
            if (line[numStalls*2] == '0'):
                    idle = 0
            for stall in range(numStalls):
                # 2 + stall*2 is used to index into the line correctly to pick up the right stalls 
                if (line[2 + stall*2] == '1'):
                    WarpStallKeeper.append(stall)
                #check for absense of idle stall, if even one warp with no idle stall, we can count this cycle
            if (WarpStallKeeper not in SchedStallKeeper):
                SchedStallKeeper.append(WarpStallKeeper)

    warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c = putStallsInRightArray(SchedStallKeeper,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,sched_num)

    # if there was a non-idle warp in the scheduler increase the cycle count of scheduler by 1
    if (idle == 0):
        counter[SM][sched_num] = counter[SM][sched_num] + 1

    return warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter

# Function to send schedulers which dont issue any warps for stall assignment
def parseSMData(keepSMData,sched0,sched1,sched2,sched3,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter):
    
    # Find the SM number
    # Check which schedulers did not issue a warp in this cycle
    dispatches = []
        
    for line in keepSMData:
        if (line[0] == "SID"):
            add = line[1].rstrip("\n")
            add = int(add)
            SM = add
        if (line[0] == "warp" and line[1] == "dispatches"):
            add = line[2].rstrip("\n")
            add = int(add)
            dispatches.append(add)

    DataSched0 = []
    DataSched1 = []
    DataSched2 = []
    DataSched3 = []
    
    # Segregate Data based on scheduler
    for line in keepSMData:
        if(line[0] == "warp" and (line[1] != 'dispatches')):
            if (line[0] == "warp" and (int(line[1]) % 4 == 0)):
                DataSched0.append(line)
            elif (line[0] == "warp"  and (int(line[1]) % 4 == 1)):
                DataSched1.append(line)
            elif (line[0] == "warp"  and (int(line[1]) % 4 == 2)):
                DataSched2.append(line)
            elif (line[0] == "warp"  and (int(line[1]) % 4 == 3)):
                DataSched3.append(line)

    # if a scheduler does not dispatch, then assign appropriate stalls
    for i in range(len(dispatches)):
        if ( dispatches[i] != 0 ):
            # Increase the counter for the SM schedular
            # Dont need to do any stall detection for this
            counter[SM][i] = counter[SM][i] + 1
        if ( dispatches[i] == 0 ):
            # Assign stalls if the schedular did not issue in this cycle
            if (i == 0):
                warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = assignStalls(sched0,DataSched0,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,i)
            elif (i == 1):
                warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = assignStalls(sched1,DataSched1,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,i)
            elif (i == 2):
                warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = assignStalls(sched2,DataSched2,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,i)
            elif (i == 3):
                warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = assignStalls(sched3,DataSched3,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter,SM,i)

    return warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter

# Function to parse the entire file and the data per SM per cycle for stall assignment
def ParseFile(file,max_warps,num_shaders,num_sched,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls):
    # Each shader starts from keyworkd SID and a cycle is increased when keyword SID0 is hit
    # When SID is hit, all the lines are appended to a list in the SID starting from keyword "Active" till "****" is encountered
    # In these lines if a shader has a dispatch all warps belonging to the shader are ignored, else the stall types in various warps is increased by 1 
    # Keep a cycle counter per SM scheduler

    #list of which warps belong to which scheduler
    sched0, sched1, sched2, sched3 = assignSchedWarps(max_warps,num_sched)

    counter = intitialiseCounter(num_shaders,num_sched)

    # adding a cycle counter for sanity check
    cycle_counter = 0
    keepSMData = []

    for line in file:
        #increase cycle counter when SID 0 is encountered
        if "SID 0" in line:
            cycle_counter = cycle_counter + 1
        linei = line
        line=line.split(' ')
       # per cycle SM data starts from SID and ends at "****" 
        if(line[0]=='SID'):
            keepSMData = []
        keepSMData.append(line)
        if("*******" in linei):
            warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = parseSMData(keepSMData,sched0,sched1,sched2,sched3,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls,counter)
            
    
    return warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter

# Function to initialise data-structures and send the file for parsing 
def GetStallData(filename,outfile,fout,numStalls):
    # read the max number of warps to initialize the list
    fin1 = open(filename,"r")
    # set max number of warps in the system
    max_warps = 64
    # set num shaders
    num_shaders = 27 # hardcoded for now
    # set num schedulers
    num_sched = 4 # hardcoded for now
    # initialize the data structure 
    # Data structure-  warp_indep_c[SM][sched][stall1][# stalls]
    # Data structure-  warp_two_c[SM][sched][stall1][stall2][# stalls]
    warp_indep_c = init_one(num_shaders,num_sched,numStalls)
    warp_two_c = init_two(num_shaders,num_sched,numStalls)
    warp_three_c = init_three(num_shaders,num_sched,numStalls)
    warp_four_c = init_four(num_shaders,num_sched,numStalls)
    warp_five_c = init_five(num_shaders,num_sched,numStalls)
    warp_six_c = init_six(num_shaders,num_sched,numStalls)
    warp_seven_c = init_seven(num_shaders,num_sched,numStalls)
    warp_eight_c = init_eight(num_shaders,num_sched,numStalls)
    warp_nine_c = init_nine(num_shaders,num_sched,numStalls)

    #Initialise final arrays (having Reduced cycles and total cycles for each stall) in which results will be kept
    # indep = init_one_final(numStalls)
    # twoStall = init_two_final(numStalls)
    # threeStall = init_three_final(numStalls)
    # fourStall = init_four_final(numStalls)
    # fiveStall = init_five_final(numStalls)
    # sixStall = init_six_final(numStalls)
    # sevenStall = init_seven_final(numStalls)
    # eightStall = init_eight_final(numStalls)
    # nineStall = init_nine_final(numStalls)

    warp_indep_c, warp_two_c, warp_three_c, warp_four_c, warp_five_c, warp_six_c, warp_seven_c, warp_eight_c, warp_nine_c, counter = ParseFile(fin1,max_warps,num_shaders,num_sched,warp_indep_c,warp_two_c,warp_three_c,warp_four_c,warp_five_c,warp_six_c,warp_seven_c,warp_eight_c,warp_nine_c,numStalls)

    # sort the independent stalls in descending order
    indep = indeps(numStalls,warp_indep_c,counter,num_shaders,num_sched)
    twoStall= twos(numStalls,warp_two_c,counter,num_shaders,num_sched)
    threeStall = threes(numStalls,warp_three_c,counter,num_shaders,num_sched)
    fourStall = fours(numStalls,warp_four_c,counter,num_shaders,num_sched)
    fiveStall = fives(numStalls,warp_five_c,counter,num_shaders,num_sched)
    sixStall = sixs(numStalls,warp_six_c,counter,num_shaders,num_sched)
    sevenStall = sevens(numStalls,warp_seven_c,counter,num_shaders,num_sched)
    eightStall = eights(numStalls,warp_eight_c,counter,num_shaders,num_sched)
    nineStall = nines(numStalls,warp_nine_c,counter,num_shaders,num_sched)

    fin1.close()
    
    return indep,twoStall,threeStall,fourStall,fiveStall,sixStall,sevenStall,eightStall,nineStall

# PRINT THE STATS!!
def printOut(indep,twoStall,threeStall,fourStall,fiveStall,sixStall,sevenStall,eightStall,nineStall,fout):
    names=['mem_str','mem_data','synco','comp_str','comp_data','control','ibufferw','imisspendingw','pendingWritew','idle']

    #independent stalls
    fout.write("\n********* INDEPENDENT STALLS ************\n")
    num = 0
    for i in range(len(indep)):
        speedup =  float(float(indep[i][2])/float(indep[i][1]))
        print(indep[i], "speedup ",speedup)
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[indep[i][0]])+" speedup "+str(round(speedup,2))+"%\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")


    fout.write("\n")

    #two stalls
    fout.write("********* TWO STALLS ************\n")
    num=0
    for i in range(len(twoStall)):
        speedup = float(twoStall[i][3])/float(twoStall[i][2])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[twoStall[i][0]])+" + "+str(names[twoStall[i][1]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    #three stalls
    fout.write("********* THREE STALLS ************\n")
    num = 0
    for i in range(len(threeStall)):
        speedup = float(threeStall[i][4])/float(threeStall[i][3])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[threeStall[i][0]])+" + "+str(names[threeStall[i][1]])+" + "+str(names[threeStall[i][2]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    #four stalls
    fout.write("********* FOUR STALLS ************\n")
    num = 0
    for i in range(len(fourStall)):
        speedup = float(fourStall[i][5])/float(fourStall[i][4])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[fourStall[i][0]])+" + "+str(names[fourStall[i][1]])+" + "+str(names[fourStall[i][2]])+" + "+str(names[fourStall[i][3]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    #five stalls
    fout.write("********* FIVE STALLS ************\n")
    num = 0
    for i in range(len(fiveStall)):
        speedup = float(fiveStall[i][6])/float(fiveStall[i][5])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[fiveStall[i][0]])+" + "+str(names[fiveStall[i][1]])+" + "+str(names[fiveStall[i][2]])+" + "+str(names[fiveStall[i][3]])+" + "+str(names[fiveStall[i][4]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    fout.write("********* SIX STALLS ************\n")
    num = 0
    for i in range(len(sixStall)):
        speedup = float(sixStall[i][7])/float(sixStall[i][6])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[sixStall[i][0]])+" + "+str(names[sixStall[i][1]])+" + "+str(names[sixStall[i][2]])+" + "+str(names[sixStall[i][3]])+" + "+str(names[sixStall[i][4]])+" + "+str(names[sixStall[i][5]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    fout.write("********* SEVEN STALLS ************\n")
    num = 0
    for i in range(len(sevenStall)):
        speedup = float(sevenStall[i][8])/float(sevenStall[i][7])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[sevenStall[i][0]])+" + "+str(names[sevenStall[i][1]])+" + "+str(names[sevenStall[i][2]])+" + "+str(names[sevenStall[i][3]])+" + "+str(names[sevenStall[i][4]])+" + "+str(names[sevenStall[i][5]])+" + "+str(names[sevenStall[i][6]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    fout.write("********* EIGHT STALLS ************\n")
    num = 0
    for i in range(len(eightStall)):
        speedup = float(eightStall[i][9])/float(eightStall[i][8])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[eightStall[i][0]])+" + "+str(names[eightStall[i][1]])+" + "+str(names[eightStall[i][2]])+" + "+str(names[eightStall[i][3]])+" + "+str(names[eightStall[i][4]])+" + "+str(names[eightStall[i][5]])+" + "+str(names[eightStall[i][6]])+" + "+str(names[eightStall[i][7]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

    fout.write("\n")

    fout.write("********* NINE STALLS ************\n")
    num = 0
    for i in range(len(nineStall)):
        speedup = float(nineStall[i][10])/float(nineStall[i][9])
        if(round(speedup,2)>1):
            num = 1
            fout.write(str(names[nineStall[i][0]])+" + "+str(names[nineStall[i][1]])+" + "+str(names[nineStall[i][2]])+" + "+str(names[nineStall[i][3]])+" + "+str(names[nineStall[i][4]])+" + "+str(names[nineStall[i][5]])+" + "+str(names[nineStall[i][6]])+" + "+str(names[nineStall[i][7]])+" + "+str(names[nineStall[i][8]])+" speedup "+str(round(speedup,2))+"\n")
    if(num == 0):
        fout.write("\n NO STALL REMOVAL GIVES >1 SPEEDUP\n")

# Main Function
def main():
    #filename="/u/ls24/rodinia/cuda/hotpot/stall_output.txt"
    outfile="fast_pred_result.txt"

    data_folder = Path("/u/ls24/rodinia/cuda/b+tree/")
    filename = data_folder / "stall_output.txt"
    
    fout=open(outfile,"w")
    numStalls=10 #Number of stalls to consider

    indep,twoStall,threeStall,fourStall,fiveStall,sixStall,sevenStall,eightStall,nineStall=GetStallData(filename,outfile,fout,numStalls)

    #print the output
    printOut(indep,twoStall,threeStall,fourStall,fiveStall,sixStall,sevenStall,eightStall,nineStall,fout)


if __name__ == "__main__":
    
    main()