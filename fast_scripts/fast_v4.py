from collections import deque
from copy import deepcopy
from pathlib import Path
import enum

# set max number of warps in the system
max_warps = 4
# set num shaders
num_shaders = 1 # hardcoded for now
# set num schedulers
num_sched = 1 # hardcoded for now
num_sched_system = 4

# we only consider the following stall reason
numStalls = 10
fullStall = [1] * 10

class Stall(enum.Enum):
   Mem_str = 0
   Mem_data = 1
   Synco = 2
   Comp_str = 3
   Comp_data = 4
   Control = 5
   IBuffer = 6
   IMiss = 7
   PendingWrite = 8
   Idle = 9

class Struct_stall_types(enum.Enum):
    mem_inst = 0,
    sp_inst = 1
    sfu_inst = 2
    tensor_inst = 3
    dp_inst = 4
    int_inst = 5
    spec_inst = 6
    alu_sfu = 7
    barrier_op = 8
    barrier_mem = 9

class Cycle:
    def __init__(self):
        self.end = False
        self.active = False
        self.issue = 0
        self.stalledOn = fullStall
        self.count = 1

        # New data
        self.cycleNumber = -1
        self.reserve_mem = -1
        self.release_mem = -1
        self.reserve_comp = -1
        self.release_comp = -1
        self.functionalUnit = -1
        self.block_id = -1
        self.structState = [1] * 7

        self.wid = -1
        self.cycle = -1
        self.line = -1

    def __eq__(self, other):
        return self.end == other.end \
                and self.active == other.active \
                and self.issue == other.issue \
                and self.stalledOn == other.stalledOn \
                and self.reserve_mem == other.reserve_mem \
                and self.release_mem == other.release_mem \
                and self.reserve_comp == other.reserve_comp \
                and self.release_comp == other.release_comp \
                and self.functionalUnit == other.functionalUnit \
                and self.block_id == other.block_id \
                and self.structState == other.structState \
                and self.wid == other.wid \
                and self.cycle == other.cycle \
                and self.line == other.line \

                

# We add one instruction to all stacks.
# Note if a warp was inactive, we assign it an idle stall
def refill_stacks():
    global total_cycles
    global warp_hit
    global cycle_counter


    line = ' '

    # Read only one cycle (until next cycle header or EOF)
    while ('CYCLE' not in line) and (line):

        line_split = line.split(' ')

        #if 'SID' not in line:
        if line_split[0] != 'SID':
            line = fin.readline()
            line_split = line.split(' ')
            continue
        
        split_line = line.split(' ')
        sid = int(split_line[1].rstrip("\n"))

        if sid >= num_shaders or sid < 0:
            line = fin.readline()
            continue

        # Read the data for the entire shader
        while '******' not in line:
            # Read the data for all warps in a scheduler
            if 'SCHEDULER' in line:
                split_line = line.split(' ')
                sched_id = int(split_line[1].rstrip("\n"))

                # TEMPORARY TO FIX OUTPUT BUG
                if 'Active' in line:
                    sched_id = 0

                if sched_id >= num_sched or sched_id < 0:
                    line = fin.readline()
                    continue

                stack_id = sid * num_sched_system + sched_id
                struct_info = []

                while 'dispatched' not in line:
                    if 'Struct avail' in line:
                        split_line = line.split(' ')[2:]
                        if('\n' in split_line):
                            split_line.remove('\n')
                        for i in range(len(split_line)):
                            struct_info.append(int(split_line[i].rstrip("\n")))


                    if 'warp dispatches' in line:
                        split_line = line.split(' ')
                        wDispatched = int(int(split_line[2].rstrip("\n")) / num_sched_system)

                    # Update the warp object with the stall information
                    else:
                        if 'warp' in line:
                            split_line = line.split(' ')
                            warp_id = int(int(split_line[1])  / num_sched_system)
                            wid = int(split_line[1])

                            # remove all warps showing idle stalls from consideration (for time being)
                            if(warp_id < int(max_warps/num_sched_system)):
                                stalls = []
                                is_idle_only = False
                                idle_stall_pres = False
                                other_stall_pres = False
                                for i in range(numStalls):
                                    stalls.append(int(split_line[2+i]))

                                if not is_idle_only:
                                    stacks[stack_id][warp_id].append(Cycle())

                                    stacks[stack_id][warp_id][-1].stalledOn = stalls

                                    # Get new scoreboard / struct data
                                    stacks[stack_id][warp_id][-1].cycleNumber = total_cycles
                                    stacks[stack_id][warp_id][-1].reserve_mem = int(split_line[2+numStalls])
                                    stacks[stack_id][warp_id][-1].release_mem = int(split_line[3+numStalls])
                                    stacks[stack_id][warp_id][-1].reserve_comp = int(split_line[4+numStalls])
                                    stacks[stack_id][warp_id][-1].release_comp = int(split_line[5+numStalls])
                                    stacks[stack_id][warp_id][-1].functionalUnit = int(split_line[6+numStalls])
                                    stacks[stack_id][warp_id][-1].block_id = int(split_line[7+numStalls])
                                    stacks[stack_id][warp_id][-1].structState = struct_info

                                    stacks[stack_id][warp_id][-1].wid = wid
                                    stacks[stack_id][warp_id][-1].cycle = total_cycles
                                    #stacks[stack_id][warp_id][-1].line = split_line

                                    stacks[stack_id][warp_id][-1].active = True

                    line = fin.readline()

                # If we exited the cycle, the currently line is '#inst dispatched'
                split_line = line.split(' ')
                nDispatched = int(split_line[2].rstrip("\n"))
                if(wDispatched < int(max_warps/num_sched_system) and nDispatched>0):
                    stacks[stack_id][wDispatched][-1].issue = nDispatched
            line = fin.readline()
        line = fin.readline()

    total_cycles += 1

    # If we reached EOF, add end signaler to all warps
    if not line:
        # Create an element at the end of each stack to signal end
        endCycle = Cycle()
        endCycle.end = True

        for k in range(num_sched * num_shaders):
            for i in range(int(max_warps / num_sched_system)):
                # If any warps have been inactive until now,
                # delete all inactive cycles from the end
                if stacks[k][i]:
                    next_cycle = stacks[k][i][-1]
                    while next_cycle.active == False:
                        stacks[k][i].pop()
                        if not stacks[k][i]:
                            break
                        next_cycle = stacks[k][i][-1]

                stacks[k][i].append(endCycle)

def are_stalls_solved(cycle, fixedStalls):
    # Ignore resolved stalls, if all stalls were resolved and no issue was done, we cannot continuously pop stalls
    cycle_temp = deepcopy(cycle)
    stall_resolved = False
    for stall in fixedStalls:
        if(cycle_temp.stalledOn[stall.value] == 1):
            stall_resolved = True
        cycle_temp.stalledOn[stall.value] = 0
    if not stall_resolved:
        return False
    else:
        for stall in Stall:
            if cycle_temp.stalledOn[stall.value] == 1:
                return False
        return True

def stalls_present(cycle, stallQuery, fixedStalls):
    # Ignore resolved stalls
    for stall in fixedStalls:
        cycle.stalledOn[stall.value] = 0
    # Return true if any is not fixed
    for stall in stallQuery:
        if cycle.stalledOn[stall.value] == 1:
            return True
    return False

def pop_next_cycle_for_warp(stack_id,index,warp_over):
    # If the top of stack i is empty, read one more cycle from file
    while not stacks[stack_id][index]:
        refill_stacks()
    # Pop the top cycle from stack (unless it is the end marker)
    if stacks[stack_id][index][0].end:
        wid = stacks[stack_id][index][0].wid
        warp_over[wid] =  warp_over[wid] + 1
        return stacks[stack_id][index][0]
    else:
        if stacks[stack_id][index][0].count > 1:
            stacks[stack_id][index][0].count -= 1
            stacks[stack_id][index][0].cycle += 1
            return stacks[stack_id][index][0]
        else:
            return stacks[stack_id][index].popleft()

def read_next_cycle_for_warp(stack_id, index, cycle_counter):
    global total_cycles
    # If the top of stack i is empty, read one more cycle from file
    while not stacks[stack_id][index]:
        refill_stacks()
    return stacks[stack_id][index][0]

def init_popped_cycles(k):
    popped_cycles = []
    for i in range(k):
        temp = []
        popped_cycles.append(temp)
    return popped_cycles

def readd(k, warp_removed, popped_cycles, counter, fixedStalls,warp_over, cycle_counter):
    readd = 0

    #Do readd
    next_cycle = read_next_cycle_for_warp(k, warp_removed, cycle_counter)
    wid = next_cycle.wid
    if next_cycle.end == False:

        #MemData
        if next_cycle.reserve_mem > next_cycle.release_mem:
                    readd = cycle_count
        else:
        # Readd cycles until last released
            badCycles = 0
            for cycle in popped_cycles:
                # While we have a register that got reserved and not released
                if cycle.cycleNumber < next_cycle.release_mem:
                    badCycles += 1
                else:
                    break
            readd = max(readd, badCycles)

        # COMP_DATA STALLS
        # If we still have the conflict, readd all cycles
        if next_cycle.reserve_comp > next_cycle.release_comp:
            readd = cycle_count
        else:
        # Readd cycles until last released
            badCycles = 0
            for cycle in popped_cycles:
                # While we have a register that got reserved and not released
                if cycle.cycleNumber < next_cycle.release_comp:
                    badCycles += 1
                else:
                    break
            readd = max(readd, badCycles)

        if next_cycle.functionalUnit == 0 and Stall.Mem_str not in fixedStalls:
            badCycles = 0
            for cycle in popped_cycles:
                # If there was no issue on this sched in cycle, if stall type was fixed stalls would have been resolved, but no dispatch due to memstr hazard
                if cycle.structState[0] == 0:
                    badCycles += 1
                else:
                    break
            readd = max(readd, badCycles)

        if next_cycle.functionalUnit > 0 and Stall.Comp_str not in fixedStalls:
            badCycles = 0
            for cycle in popped_cycles:
                # If there was no issue on this sched in cycle, if stall type was fixed stalls would have been resolved, but no dispatch due to compstr hazard
                if(next_cycle.functionalUnit != Struct_stall_types.barrier_op.value and  next_cycle.functionalUnit != Struct_stall_types.barrier_mem.value):
                    if (cycle.structState[next_cycle.functionalUnit] == 0):
                        badCycles += 1
                    else:
                        break
            readd = max(readd, badCycles)

        # Number of cycles removed
        removed_cycles = counter - readd

        # Since one cycle has to move forward
        if (readd > 0):
            cycle = popped_cycles[removed_cycles].cycle
        else:
            cycle = popped_cycles[removed_cycles-1].cycle

        #if(removed_cycles == 0):
        #    cycle = cycle + 1

        # Add said cycles as full (unfixable) stalls
        if(readd > 0):
            readd_cycle = Cycle()
            readd_cycle.active = True
            #readd_cycle.count = readd - 1
            readd_cycle.count = readd
            readd_cycle.wid = wid
            readd_cycle.cycle = cycle
            readd_cycle.cycleNumber = cycle

def cycle(fixedStalls,dual_cycle,num_issues,warp_cycles,removed_cycles,warp_over,last_cycle,fout,last_warp_cycle):
    end = True
    global cycle_counter
    global warp_call
    global warp_removed
    global total_cycles
    global start_cycle

    warp_0_removed = False

    for k in range(num_sched * num_shaders):
        issued = False

        cycle_counter = cycle_counter + 1

        cycle_consider = -1
        warp_consider = 0
        issue_counter = 0

        #check if there is at least one warp that does not end
        for i in range(len(stacks[k])):
            next_cycle = read_next_cycle_for_warp(k, i, cycle_counter)
            # tester
            wid = next_cycle.wid
            if(cycle_counter == 1):
                start_cycle[wid] = next_cycle.cycleNumber
            if not next_cycle.end and (i==warp_consider):
                end = False
                cycle_consider = next_cycle.cycleNumber
                last_cycle[next_cycle.wid] = cycle_counter
            if next_cycle.end and (i==warp_consider):
                warp_consider = warp_consider + 1
            if next_cycle.issue > 0:
                issue_counter = issue_counter + 1
            if(wid == 4):
                #if(last_warp_cycle != next_cycle.cycleNumber-1):
                #    fout.write("Tot_cycle "+str(cycle_consider)+" Last cycle "+str(last_warp_cycle)+" pres cycle "+str(next_cycle.cycleNumber)+"\n")
                last_warp_cycle = next_cycle.cycleNumber
        if (issue_counter > 1):
            dual_cycle = dual_cycle + issue_counter - 1
        

        #check if there is an issuing warp among all warps
        for i in range(len(stacks[k])):
            next_cycle = read_next_cycle_for_warp(k, i, cycle_counter)
            wid = next_cycle.wid
            if next_cycle.issue > 0  and next_cycle.cycleNumber <= cycle_consider:
            #if next_cycle.issue > 0:
                issued = True
                num_issues = num_issues + 1

        #if issued is true pop one cycle from all warps
        if issued and not end:
            for i in range(len(stacks[k])):
                next_cycle = read_next_cycle_for_warp(k, i, cycle_counter)
                wid = next_cycle.wid
                if(not next_cycle.end and next_cycle.cycleNumber <= cycle_consider):
                    pop_next_cycle_for_warp(k, i,warp_over)
                    warp_cycles[wid] = warp_cycles[wid] + 1
                    if(wid == 63):
                        warp_call = warp_call + 1
                    if(wid == 4):
                        warp_0_removed = True
                else:
                    if(wid == 4):
                        warp_0_removed = True

        
        issuing_cycle = False
        issuing_warp = -1
        warps_done = []

        undergo = True

        if not issued and not end:
            for i in range(len(stacks[k])):
                next_cycle = read_next_cycle_for_warp(k, i, cycle_counter)
                wid = next_cycle.wid
                if next_cycle.cycleNumber <= cycle_consider:
                    wid = next_cycle.wid
                    popped_cycles = []
                    counter = 0
                    start = True
                    if are_stalls_solved(next_cycle, fixedStalls) and not issuing_cycle and undergo:
                        issuing_cycle = True
                        issued = True
                        issuing_warp = i
                        warp_issuing = False
                        warps_done.append(i)
                        start_removing = next_cycle.cycleNumber
                        next_removing = next_cycle.cycleNumber
                        cycles_removed = 0
                        counter = 0
                        while are_stalls_solved(next_cycle, fixedStalls) and next_cycle.end == False and not warp_issuing and (next_removing == start_removing + cycles_removed):
                            if(next_removing == start_removing + cycles_removed):
                                next_cycle = pop_next_cycle_for_warp(k, i,warp_over)
                                if(wid == 4):
                                    warp_0_removed = True
                                popped_cycles.append(next_cycle)
                                warp_cycles[wid] = warp_cycles[wid] + 1
                                counter = counter + 1
                                #if(start == False): 
                                removed_cycles[wid] = removed_cycles[wid] + 1
                            cycles_removed = cycles_removed + 1
                            start = False
                            next_cycle = read_next_cycle_for_warp(k, i, cycle_counter)
                            next_removing = next_cycle.cycleNumber
                            wid = next_cycle.wid
                        # go for readd
                        #readd(k, i, popped_cycles, counter, fixedStalls,warp_over, cycle_counter)
                        # remove a cycle to show real progress
                        next_cycle = pop_next_cycle_for_warp(k, i,warp_over)
                    else:
                        if(not next_cycle.end):
                            wid = next_cycle.wid
                            pop_next_cycle_for_warp(k, i,warp_over)

                            warp_cycles[wid] = warp_cycles[wid] + 1
                            if(wid == 63):
                                warp_call = warp_call + 1
                            if(wid == 4):
                                warp_0_removed = True

    return end, dual_cycle, num_issues, warp_cycles, removed_cycles, warp_over, last_cycle, fout, last_warp_cycle



# Function to initialise data-structures and send the file for parsing
def profileStalls(filename, fixedStalls,dual_cycle,fout):
    global fin
    global stacks
    global total_cycles
    global warp_hit 
    global warp_call
    global cycle_counter
    global warp_removed
    global start_cycle

    cycle_counter = 0
    warp_removed = 0
    last_warp_cycle = -1

    # list to store #cycles per warp
    warp_cycles = []
    #list to store #cycles removed per warp
    removed_cycles = []
    # cycles left after stall removed
    cycles_left = [] 
    # cannot remove warp, warp already over end this madness
    warp_over = []
    # check last cycle for each warp
    last_cycle = []
    # check start cycle for each warp
    start_cycle = []
    # calculate warp_hit
    warp_hit = 0
    warp_call = 0

    for i in range(max_warps+1):
        warp_cycles.append(0)
        removed_cycles.append(0)
        cycles_left.append(0)
        warp_over.append(0)
        start_cycle.append(0)
        last_cycle.append(0)

    num_issues = 0

    fin = open(filename,"r")
    # The stacks of "cycles" for each warp (per SM scheduler)
    stacks = []
    for i in range(num_sched * num_shaders):
        sched_stack = []
        for j in range(int(max_warps / num_sched_system)):
            sched_stack.append(deque([]))
        stacks.append(sched_stack)

    # We read until past the first cycle marker to make our reading work
    line = ''
    while 'CYCLE' not in line:
        line = fin.readline()

    # For now the script only does 1 SM and 1 Scheduler for simplicity
    # TODO: Extend to take minimum speedup among all

    #print("START ",warp_cycles[63]," ",removed_cycles[63]," ",cycles_left[63])

    done = False
    cycles = 0
    total_cycles = 0
    while not done:
        done,dual_cycle,num_issues,warp_cycles,removed_cycles,warp_over,last_cycle,fout,last_warp_cycle = cycle(fixedStalls,dual_cycle,num_issues,warp_cycles,removed_cycles,warp_over,last_cycle,fout,last_warp_cycle)
        cycles += 1

    cycles -= 1 # We were done in the previous cycle
    for stall in fixedStalls:
        print(stall.name, end=' ', flush=True)
    print("Total cycles ",total_cycles, flush=True)
    print("Post stall removal ",cycles, flush=True)
    print("cycles removed ",total_cycles - cycles, flush=True)
    print("num issues ",num_issues, flush=True)

    fout.write("total cycles "+str(total_cycles)+"\n")
    fout.write("cycles removed "+str(total_cycles - cycles)+"\n")
    fout.write("cycles left "+str(cycles)+"\n")
    for i in range(len(warp_cycles)):
        fout.write("i "+str(i)+" cycles "+str(warp_cycles[i])+" removed "+str(removed_cycles[i])+" left "+str(warp_cycles[i] - removed_cycles[i])+" start "+str(start_cycle[i])+" end "+str(last_cycle[i])+" \n")

    fin.close()
    return dual_cycle, fout

# Main Function
def main():
    #data_folder = Path("/scratch/ls24/rodinia/cuda/nn")
    #filename = data_folder / "stall_output.txt"

    filename = "temp"
    fout=open('nn_out_test8',"w")

    fout.write("OPENING NN\n")

    #Number of cycles with more than 1 issue
    dual_cycle = 0
    fixedStalls = [Stall.Mem_data]
    print("MEMDATA")
    fout.write("MEMDATA\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls,dual_cycle,fout)
    print("DUAL CYCLE ",dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Mem_str]
    print("MEMSTR")
    fout.write("MEMSTR\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.Synco]
    print("SYNCO")
    fout.write("SYNCO\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.Comp_str]
    print("COMPSTR")
    fout.write("COMPSTR\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.Comp_data]
    print("COMPDATA")
    fout.write("COMPDATA\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.Control]
    print("CONTROL")
    fout.write("CONTROL\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.IBuffer]
    print("IBUFFER")
    fout.write("IBUFFER\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.PendingWrite]
    print("PENDINGW")
    fout.write("PENDINGW\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    dual_cycle = 0
    fixedStalls = [Stall.Idle]
    print("IDLE")
    fout.write("IDLE\n")
    dual_cycle,fout = profileStalls(filename, fixedStalls, dual_cycle,fout)

    fout.close()

if __name__ == "__main__":
    main()
