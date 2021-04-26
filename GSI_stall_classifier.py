import enum
from pathlib import Path

class Stall(enum.Enum):
   No_stall = 0
   Mem_str = 2
   Mem_data = 3
   Synco = 4
   Comp_str = 5
   Comp_data = 6
   Control = 7
   IBuffer = 8
   PWrite = 10
   Idle = 11

def getstats(inf):
    cycles=0; #number of cycles
    no_stall=0
    mem_str=0
    mem_data=0
    synchro=0
    comp_str=0
    comp_data=0
    control=0
    ibuffer = 0
    pwrite = 0
    idle=0
    other=0

    reading_cycle = False
    cycle_stall = Stall.No_stall
    for line in inf:
        line = line.split(' ')
        if('CYCLE' in line[0]):
            if cycle_stall == Stall.Mem_str:
                mem_str += 1
            if cycle_stall == Stall.Mem_data:
                mem_data += 1
            if cycle_stall == Stall.Synco:
                synchro += 1
            if cycle_stall == Stall.Comp_str:
                comp_str += 1
            if cycle_stall == Stall.Comp_data:
                comp_data += 1
            if cycle_stall == Stall.Control:
                control += 1
            if cycle_stall == Stall.IBuffer:
                ibuffer += 1
            if cycle_stall == Stall.PWrite:
                pwrite += 1
            if cycle_stall == Stall.Idle:
                idle += 1

            cycles=cycles+1
            cycle_stall = Stall.No_stall
            reading_cycle = True
        
        if reading_cycle and line[0] == 'warp' and len(line) > 7:
            # Algorithm 1 Instruction Stall Classification
            warp_stall = Stall.No_stall
            if '1' in line[Stall.Comp_str.value]:
                warp_stall = Stall.Comp_str
            if '1' in line[Stall.Comp_data.value]:
                warp_stall = Stall.Comp_data
            if '1' in line[Stall.Mem_str.value]:
                warp_stall = Stall.Mem_str
            if '1' in line[Stall.Mem_data.value]:
                warp_stall = Stall.Mem_data
            if '1' in line[Stall.Synco.value]:
                warp_stall = Stall.Synco
            if '1' in line[Stall.Control.value]:
                warp_stall = Stall.Control
            if '1' in line[Stall.PWrite.value]:
                warp_stall = Stall.PWrite
            if '1' in line[Stall.IBuffer.value]:
                warp_stall = Stall.IBuffer
            if '1' in line[Stall.Idle.value]:
                warp_stall = Stall.Idle

            # Algorithm 2 Issue Cycle Stall Classification
            if cycle_stall == Stall.Mem_str or warp_stall == Stall.Mem_str:
                cycle_stall = Stall.Mem_str
                continue
            if cycle_stall == Stall.Mem_data or warp_stall == Stall.Mem_data:
                cycle_stall = Stall.Mem_data
                continue
            if cycle_stall == Stall.Synco or warp_stall == Stall.Synco:
                cycle_stall = Stall.Synco
                continue
            if cycle_stall == Stall.Comp_str or warp_stall == Stall.Comp_str:
                cycle_stall = Stall.Comp_str
                continue
            if cycle_stall == Stall.Comp_data or warp_stall == Stall.Comp_data:
                cycle_stall = Stall.Comp_data
                continue
            if cycle_stall == Stall.Control or warp_stall == Stall.Control:
                cycle_stall = Stall.Control
                continue
            if cycle_stall == Stall.IBuffer or warp_stall == Stall.IBuffer:
                cycle_stall = Stall.IBuffer
                continue
            if cycle_stall == Stall.PWrite or warp_stall == Stall.PWrite:
                cycle_stall = Stall.PWrite
                continue            
            if cycle_stall == Stall.Idle or warp_stall == Stall.Idle:
                cycle_stall = Stall.Idle
                continue

        if reading_cycle and len(line) > 1 and line[1] == 'dispatched':
            if '0' not in line[2]:
                cycle_stall = Stall.No_stall
                reading_cycle = False
                continue
            else:
                if cycle_stall != Stall.Idle and cycle_stall != Stall.No_stall:
                    reading_cycle = False
                    continue


    print("Total Cycles " +str(cycles))
    print("MemStr: "+str(mem_str))
    print("MemData: "+str(mem_data))
    print("Synchro "+str(synchro))
    print("CompStr: "+str(comp_str))
    print("CompData: "+str(comp_data))
    print("Control: "+str(control))
    print("IBuffer: "+str(ibuffer))
    print("PWrite: "+str(pwrite))
    print("Idle: "+str(idle))
    print("Other: "+str(other))

def main():
    data_folder = Path("/scratch/ls24/shoc/src/cuda/level1/bfs")
    filename = data_folder / "stall_output.txt"

    fin=open(filename,"r")

    getstats(fin)



if __name__ == "__main__":
    main()
