from collections import deque
from copy import deepcopy
from pathlib import Path
import enum

# set max number of warps in the system
max_warps = 64
# set num shaders
num_shaders = 1 # hardcoded for now
# set num schedulers
num_sched = 1 # hardcoded for now

# we only consider the following stall reasons
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

# We add one instruction to all stacks.
# Note if a warp was inactive, we assign it an idle stall
def refill_stacks():
    global total_cycles

    # We fill an empty cycle in all stacks (which we then fill)
    # Initially marked as inactive
    for k in range(num_sched * num_shaders):
        for i in range(int(max_warps / num_sched)):
            stacks[k][i].append(Cycle())

    line = ' '
    # Read only one cycle (until next cycle header or EOF)
    while ('CYCLE' not in line) and (line):
        if 'SID' not in line:
            line = fin.readline()
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

                if sched_id >= num_shaders or sched_id < 0:
                    line = fin.readline()
                    continue

                stack_id = sid * num_sched + sched_id
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
                        wDispatched = int(int(split_line[2].rstrip("\n")) / num_sched)

                    # Update the warp object with the stall information
                    else:
                        if 'warp' in line:
                            split_line = line.split(' ')
                            warp_id = int(int(split_line[1])  / num_sched)

                            if(warp_id < max_warps):
                                stalls = []
                                for i in range(numStalls):
                                    stalls.append(int(split_line[2+i]))
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

                                stacks[stack_id][warp_id][-1].active = True
                    
                    line = fin.readline()

                # If we exited the cycle, the currently line is '#inst dispatched'
                split_line = line.split(' ')
                nDispatched = int(split_line[2].rstrip("\n"))
                if(wDispatched < max_warps):
                    stacks[stack_id][wDispatched][-1].issue = nDispatched
            line = fin.readline()
        line = fin.readline()

    total_cycles += 1

    # Compress elements in stack if they are equal
    # This is really useful for idle or inactive warps
    for k in range(num_sched * num_shaders):
        for i in range(int(max_warps / num_sched)):
            if len(stacks[k][i]) >= 2 and (stacks[k][i][-1] == stacks[k][i][-2]):
                stacks[k][i].pop()
                stacks[k][i][-1].count += 1
    
    # If we reached EOF, add end signaler to all warps
    if not line:
        # Create an element at the end of each stack to signal end
        endCycle = Cycle()
        endCycle.end = True

        for k in range(num_sched * num_shaders):
            for i in range(int(max_warps / num_sched)):
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
    # Ignore resolved stalls
    for stall in fixedStalls:
        cycle.stalledOn[stall.value] = 0
    # Return false if any is not fixed
    for stall in Stall:
        if cycle.stalledOn[stall.value] == 1:
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

def pop_next_cycle_for_warp(stack_id,index):
    # If the top of stack i is empty, read one more cycle from file
    if not stacks[stack_id][index]:
        refill_stacks()

    # Pop the top cycle from stack (unless it is the end marker)
    if stacks[stack_id][index][0].end:
        return stacks[stack_id][index][0]
    else:
        if stacks[stack_id][index][0].count > 1:
            stacks[stack_id][index][0].count -= 1
            return stacks[stack_id][index][0]
        else:
            return stacks[stack_id][index].popleft()

def read_next_cycle_for_warp(stack_id, index):
    # If the top of stack i is empty, read one more cycle from file
    if not stacks[stack_id][index]:
        refill_stacks()
    # Read the top cycle from stack
    return stacks[stack_id][index][0]

def init_popped_cycles(k):
    popped_cycles = []
    for i in range(k):
        temp = []
        popped_cycles.append(temp)
    return popped_cycles

def cycle(fixedStalls,dual_cycle):
    end = True
    for k in range(num_sched * num_shaders):
        issued = False

        popped_cycles = init_popped_cycles(len(stacks[k]))

        issue_counter = 0
        #check if there is at least one warp that does not end
        for i in range(len(stacks[k])):
            next_cycle = read_next_cycle_for_warp(k, i)
            if not next_cycle.end:
                end = False
            if next_cycle.issue > 0:
                issue_counter = issue_counter + 1
        if (issue_counter > 1):
            dual_cycle = dual_cycle + issue_counter - 1

        #check if there is an issuing warp among all warps
        for i in range(len(stacks[k])):
            next_cycle = read_next_cycle_for_warp(k, i)
            if next_cycle.issue > 0:
                issued = True

        #if issued is true pop one cycle from all warps
        if issued and not end:
            for i in range(len(stacks[k])):
                pop_next_cycle_for_warp(k, i)


        cycle_count = 0
        #if a warp shows a removed stall, pop all warps in the cycle
        if not issued and not end:
            for i in range(len(stacks[k])):
                next_cycle = read_next_cycle_for_warp(k, i)
                if are_stalls_solved(next_cycle, fixedStalls):
                    issued = True
                    issuing_cycle = False
                    warp_removed = i
                    cycle_count = 0
                while are_stalls_solved(next_cycle, fixedStalls) and not issuing_cycle:
                    cycle_count = cycle_count + 1
                    for j in range(len(stacks[k])):
                        next_pop = pop_next_cycle_for_warp(k, j)
                        #if(cycle_count > 1):
                        popped_cycles[j].append(next_pop)
                        if next_pop.issue > 0:
                            issuing_cycle = True
                    next_cycle = read_next_cycle_for_warp(k, i)
                if issued:
                    break

        # for count in range(cycle_count-1):
        #     for j in range(len(stacks[k])):
        #         if j!= warp_removed:
        #             stacks[k][j].appendleft(popped_cycles[j][count])

        readd = 0
        #Do readd
        if(cycle_count > 0):
            next_cycle = read_next_cycle_for_warp(k, warp_removed)
            if next_cycle.end == False:
                #MemData
                if next_cycle.reserve_mem > next_cycle.release_mem:
                            readd = cycle_count
                else:
                # Readd cycles until last released
                    badCycles = 0
                    for cycle in popped_cycles[warp_removed]:
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
                    for cycle in popped_cycles[warp_removed]:
                        # While we have a register that got reserved and not released
                        if cycle.cycleNumber < next_cycle.release_comp:
                            badCycles += 1
                        else:
                            break
                    readd = max(readd, badCycles)

                if next_cycle.functionalUnit == 0 and Stall.Mem_str not in fixedStalls:
                    badCycles = 0
                    for cycle in popped_cycles[warp_removed]:
                        # If there was no issue on this sched in cycle, if stall type was fixed stalls would have been resolved, but no dispatch due to memstr hazard
                        if cycle.structState[0] == 0:
                            badCycles += 1
                        else:
                            break
                    readd = max(readd, badCycles)

                if next_cycle.functionalUnit > 0 and Stall.Comp_str not in fixedStalls:
                    badCycles = 0
                    for cycle in popped_cycles[warp_removed]:
                        # If there was no issue on this sched in cycle, if stall type was fixed stalls would have been resolved, but no dispatch due to compstr hazard
                        if(next_cycle.functionalUnit != Struct_stall_types.barrier_op.value and  next_cycle.functionalUnit != Struct_stall_types.barrier_mem.value):
                            if (cycle.structState[next_cycle.functionalUnit] == 0):
                                badCycles += 1
                            else:
                                break
                    readd = max(readd, badCycles)

                # Add said cycles as full (unfixable) stalls
                readd_cycle = Cycle()
                readd_cycle.active = True
                readd_cycle.count = readd
                #readd = 0 #Sanity check
                if readd > 0:
                    stacks[k][i].appendleft(readd_cycle)
                
                for count in range(readd):
                   for j in range(len(stacks[k])):
                       if j!= warp_removed:
                           stacks[k][j].appendleft(popped_cycles[j][count])

        # no cycles could be removed
        if not issued and not end:
            for i in range(len(stacks[k])):
                pop_next_cycle_for_warp(k, i)

    return end, dual_cycle



# Function to initialise data-structures and send the file for parsing
def profileStalls(filename, fixedStalls,dual_cycle):
    global fin
    global stacks
    global total_cycles

    fin = open(filename,"r")
    # The stacks of "cycles" for each warp (per SM scheduler)
    stacks = []
    for i in range(num_sched * num_shaders):
        sched_stack = []
        for j in range(int(max_warps / num_sched)):
            sched_stack.append(deque([]))
        stacks.append(sched_stack)

    # We read until past the first cycle marker to make our reading work
    line = ''
    while 'CYCLE' not in line:
        line = fin.readline()

    # For now the script only does 1 SM and 1 Scheduler for simplicity
    # TODO: Extend to take minimum speedup among all

    done = False
    cycles = 0
    total_cycles = 0
    while not done:
        done,dual_cycle = cycle(fixedStalls,dual_cycle)
        cycles += 1

    cycles -= 1 # We were done in the previous cycle
    for stall in fixedStalls:
        print(stall.name, end=' ')
    print("Total cycles ",total_cycles)
    print("Post stall removal ",cycles)
    print("cycles removed ",total_cycles - cycles)
    fin.close()
    return dual_cycle

# Main Function
def main():
    data_folder = Path("/scratch/ls24/rodinia/cuda/lavaMD")
    filename = data_folder / "stall_output.txt"

    #filename = "result_v3.txt"

    # Number of cycles with more than 1 issue
    dual_cycle = 0
    fixedStalls = [Stall.Mem_data]
    print("MEMDATA")
    dual_cycle = profileStalls(filename, fixedStalls,dual_cycle)
    print("DUAL CYCLE ",dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Mem_str]
    print("MEMSTR")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Synco]
    print("SYNCO")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Comp_str]
    print("COMPSTR")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Comp_data]
    print("COMPDATA")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Control]
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.IBuffer]
    print("IBUFFER")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.PendingWrite]
    print("PENDINGW")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

    dual_cycle = 0
    fixedStalls = [Stall.Idle]
    print("IDLE")
    dual_cycle = profileStalls(filename, fixedStalls, dual_cycle)

if __name__ == "__main__":
    main()