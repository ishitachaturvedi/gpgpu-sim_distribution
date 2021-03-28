from collections import deque
from copy import deepcopy
from pathlib import Path
import enum

# set max number of warps in the system
max_warps = 64
# set num shaders
num_shaders = 27 # hardcoded for now
# set num schedulers
num_sched = 4 # hardcoded for now

# we only consider the following stall reasons
numStalls = 10
fullStall = [1] * 10
class Stall(enum.Enum):
   Mem_str = 0
   Mem_data
   Synco
   Comp_str
   Comp_data
   Control
   IBuffer
   IMiss
   PendingWrite
   Idle

class Cycle:
    def __init__(self)
        self.end = False
        self.active = False
        self.issue = 0
        self.stalledOn = fullStall

# We add one instruction to all stacks.
# Note if a warp was inactive, we assign it an idle stall
def refill_stacks():
    # We fill an empty cycle in all stacks (which we then fill)
    # Initially marked as inactive
    for k in range(num_shaders * num_sched):
        for i in range(max_warps):
            stacks[k][i].append(Cycle())

    line = ''
    # Read only one cycle (until next cycle header or EOF)
    while ('CYCLE' not in line) and (line):
        if 'SID' not in line:
            line = fin.readline()
            continue
        
        split_line = line.split(' ')
        sid = int(split_line[1].rstrip("\n"))

        # For now, skip all non-zero shaders
        if sid != 0:
            line = fin.readline()
            continue        

        # Read the data for the entire shader
        while '******' not in line:
            # Read the data for all warps in a scheduler
            if 'SCHEDULER' in line:

                split_line = line.split(' ')
                sched_id = int(split_line[1].rstrip("\n"))
                stack_id = sid * num_sched + sched_id

                # For now, skip all non-zero schedulers
                if sched_id != 0:
                    line = fin.readline()
                    continue

                while 'dispatched' not in line:
                    # Update the warp object with the stall information
                    if 'warp' in line:
                        split_line = line.split(' ')
                        warp_id = int(split_line[1])
                       
                        stalls = []
                        for i in range(numStalls):
                            stalls.append(int(split_line[2+2*i]))                                
                        stacks[stack_id][warp_id][-1].stalledOn = stalls
                        stacks[stack_id][warp_id][-1].active = True

                    if 'dispatches' in line:
                        split_line = line.split(' ')
                        wDispatched = int(split_line[2].rstrip("\n"))
                    
                    line = fin.readline()

                # If we exited the cycle, the currently line is '#inst dispatched'
                split_line = line.split(' ')
                nDispatched = int(split_line[2].rstrip("\n"))

                stacks[stack_id][wDispatched][-1].issue = nDispatched

            line = fin.readline()

        line = fin.readline()
    
    # If we reached EOF, add end signaler to all warps
    if not line: 
        # Create an element at the end of each stack to signal end
        endCycle = Cycle()
        endCycle.end = True

        for k in range(num_shaders * num_sched):
            for i in range(max_warps):
                # If any warps have been inactive until now,
                # delete all inactive cycles from the end
                if stacks[k][i]:
                    next_cycle = stacks[k][-1]
                    while next_cycle.active == False:
                        stacks[k][i].popLeft()
                        next_cycle = stacks[k][-1]

                stacks[k][i].append(endCycle)
        
def are_stalls_solved(cycle, fixedStalls):
    # Ignore resolved stalls
    for stall in stalls:
        cycle.stalledOn[stall] = 0
    # Return false if any is not fixed
    for stall in Stall:
        if cycle.stalledOn[stall.value] == 1:
            return False
    return True

def pop_next_cycle_for_warp(stack_id,index):
    # If the top of stack i is empty, read one more cycle from file
    if stacks[stack_id][index]:
        refill_stacks()

    # Pop the top cycle from stack (unless it is the end marker)
    if stacks[stack_id][index][0].end:
        return stacks[stack_id][index][0]
    else:
        stacks[stack_id][index].pop()

def read_next_cycle_for_warp(stack_id, index):
    # If the top of stack i is empty, read one more cycle from file
    if stacks[stack_id][index]:
        refill_stacks()        
    # Read the top cycle from stack
    return stacks[stack_id][index][0]

def cycle(fixedStalls):
    end = True

    for k in range(num_sched * num_shaders):
        issued = False
        # Find issuing warp
        for i in range(len(stacks[k])):
            next_cycle = get_next_cycle_for_warp(k, i)

            # Mark at least one warp has not finished
            if not next_cycle.end:
                end = False
            
            # If warp issued, pop top cycle out of all warps
            # stack (unless those are also issues due to stacks shifting)
            if next_cycle.issue > 0:
                issued = True
                next_cycle.issue = 0 # Mark as 0 so it is also popped
                for j in range(len(stacks[k])):
                    next_cycle = get_next_cycle_for_warp(k, j)
                    if next_cycle.issue == 0:
                        pop_next_cycle_for_warp(k, j)
                break
        
        # If we did not issue, then we check if we have fixed a
        # stall condition and can issue a different warp
        if not issued and not end:
            for i in range(len(stacks[k])):
                next_cycle = get_next_cycle_for_warp(k, i)
                
                if are_stalls_solved(next_cycle, fixedStalls):
                    issued = True
                    # Remove 1 cycle from everyone to indicate
                    # 1 cycle of progress
                    for j in range(len(stacks[k])):
                        pop_next_cycle_for_warp(k, j)

                    # Remove cycles in this same warp until the
                    # instruction with the resolved stall was issued
                    counter = 0
                    while next_cycle.issue == 0:
                        pop_next_cycle_for_warp(k, i)
                        counter += 1

                    # Decide whether some cycles need to be added into the stack
                    readd = 0
                    next_cycle = read_next_cycle_for_warp(k, i)
                    if next_cycle.end == False and not are_stalls_solved(next_cycle, fixedStalls):
                        readd = counter

                    # Add said cycles as full stalls
                    readd_cycle = Cycle()
                    readd_cycle.active = True
                    for j in range(readd):
                        stacks[k][i].appendLeft(readd_cycle)
                    break
        
        # If we still did not issue, we top the top of every warp's stack
        # to indicate one cycle has passed
        if not issued and not end:
            for i in range(len(stacks[k])):
                pop_next_cycle_for_warp(i)

    return end

        

# Function to initialise data-structures and send the file for parsing 
def profileStalls(filename, fixedStalls):
    # read the max number of warps to initialize the list
    global fin = open(filename,"r")

    # We read until past the first cycle marker to make our reading work
    line = ''
    while 'CYCLE' not in line:
        line = fin.readline()

    # For now the script only does 1 SM and 1 Scheduler for simplicity
    # TODO: Extend to take minimum speedup among all

    # The stacks of "cycles" for each warp
    global stacks = [ ([deque([])] * max_warps) ] * (num_shaders * num_sched)

    done = False
    cycles = 0
    while not done:
        done = cycle(fixedStalls)
        cycles += 1

    for stall in fixedStalls:
        print(stall.name + " ")
    print(cycles)
    fin.close()

# Main Function
def main():
    outfile="fast_pred_result.txt"

    data_folder = Path("/u/ls24/rodinia/cuda/nn/")
    filename = data_folder / "stall_output.txt"
    
    fixedStalls = [Enum.Mem_data]
    profileStalls(filename, fixedStalls)

if __name__ == "__main__":
    main()