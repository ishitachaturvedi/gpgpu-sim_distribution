from collections import deque
from copy import deepcopy
from pathlib import Path
import enum

# set max number of warps in the system
max_warps = 32
# set num shaders
num_shaders = 27 # hardcoded for now
# set num schedulers
num_sched = 4 # hardcoded for now

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

class Cycle:
    def __init__(self):
        self.end = False
        self.active = False
        self.issue = 0
        self.stalledOn = fullStall
        self.count = 1
    
    def __eq__(self, other):
        return self.end == other.end \
                and self.active == other.active \
                and self.issue == other.issue \
                and self.stalledOn == other.stalledOn

# We add one instruction to all stacks.
# Note if a warp was inactive, we assign it an idle stall
def refill_stacks():
    global total_cycles
    total_cycles += 1

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

                stack_id = sid * num_sched + sched_id

                if sched_id >= num_sched or sched_id < 0:
                    line = fin.readline()
                    continue

                while 'dispatched' not in line:
                    if 'warp dispatches' in line:
                        split_line = line.split(' ')
                        wDispatched = int(int(split_line[2].rstrip("\n")) / num_sched)

                    # Update the warp object with the stall information
                    else:
                        if 'warp' in line:
                            split_line = line.split(' ')
                            warp_id = int(int(split_line[1].rstrip("\n")) / num_sched)

                            stalls = []
                            for i in range(numStalls):
                                stalls.append(int(split_line[2+2*i]))
                            stacks[stack_id][warp_id][-1].stalledOn = stalls
                            stacks[stack_id][warp_id][-1].active = True
                    
                    line = fin.readline()

                # If we exited the cycle, the currently line is '#inst dispatched'
                split_line = line.split(' ')
                nDispatched = int(split_line[2].rstrip("\n"))
                stacks[stack_id][wDispatched][-1].issue = nDispatched
            line = fin.readline()
        line = fin.readline()

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

def cycle(fixedStalls):
    end = True

    for k in range(num_sched * num_shaders):
        issued = False
        # Find issuing warp
        for i in range(len(stacks[k])):
            next_cycle = read_next_cycle_for_warp(k, i)

            # Mark at least one warp has not finished
            if not next_cycle.end:
                end = False
            
            # If warp issued, pop top cycle out of all warps
            # stack (unless those are also issues due to stacks shifting)
            if next_cycle.issue > 0:
                issued = True
                next_cycle.issue = 0 # Mark as 0 so it is also popped
                for j in range(len(stacks[k])):
                    next_cycle = read_next_cycle_for_warp(k, j)
                    if next_cycle.issue == 0:
                        pop_next_cycle_for_warp(k, j)
                break
        
        # If we did not issue, then we check if we have fixed a
        # stall condition and can issue a different warp
        if not issued and not end:
            for i in range(len(stacks[k])):
                next_cycle = read_next_cycle_for_warp(k, i)
                
                if next_cycle.active == True and are_stalls_solved(next_cycle, fixedStalls):                  
                    issued = True
                    # Remove 1 cycle from everyone to indicate
                    # 1 cycle of progress
                    for j in range(len(stacks[k])):
                        pop_next_cycle_for_warp(k, j)

                    # Remove cycles in this same warp until the
                    # instruction with the resolved stall was issued
                    counter = 0
                    while next_cycle.end == False and next_cycle.issue == 0:
                        next_cycle = pop_next_cycle_for_warp(k, i)
                        counter += 1

                    # Decide whether some cycles need to be added into the stack
                    readd = 0
                    next_cycle = read_next_cycle_for_warp(k, i)
                    if next_cycle.end == False and not are_stalls_solved(next_cycle, fixedStalls):
                        readd = counter

                    # Add said cycles as full stalls
                    readd_cycle = Cycle()
                    readd_cycle.active = True
                    readd_cycle.count = readd
                    if readd > 0:
                        stacks[k][i].appendleft(readd_cycle)
                    break
        
        # If we still did not issue, we pop the top of every warp's stack
        # to indicate one cycle has passed
        if not issued and not end:
            for i in range(len(stacks[k])):
                pop_next_cycle_for_warp(k, i)

    return end

        

# Function to initialise data-structures and send the file for parsing 
def profileStalls(filename, fixedStalls):
    global fin
    global stacks
    global total_cycles
    
    fin = open(filename,"r")
    # The stacks of "cycles" for each warp (per SM scheduler)
    stacks = []
    for i in range(num_shaders * num_sched):
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
        done = cycle(fixedStalls)
        cycles += 1

    cycles -= 1 # We were done in the previous cycle
    for stall in fixedStalls:
        print(stall.name, end=' ')
    print(total_cycles - cycles)
    fin.close()

# Main Function
def main():
    data_folder = Path("/u/ls24/rodinia/cuda/nn/")
    filename = data_folder / "stall_output.txt"
    
    fixedStalls = [Stall.Mem_data]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Mem_str]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Synco]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Comp_str]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Comp_data]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Control]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.IBuffer]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.PendingWrite]
    profileStalls(filename, fixedStalls)

    fixedStalls = [Stall.Idle]
    profileStalls(filename, fixedStalls)

if __name__ == "__main__":
    main()