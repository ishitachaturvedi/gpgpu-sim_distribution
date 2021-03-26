import enum

class Stall(enum.Enum):
   No_stall = 0
   Mem_str = 2
   Mem_data = 3
   Synco = 4
   Comp_str = 5
   Comp_data = 6
   Control = 7
   Idle = 11

def getstats(inf,outf):
    cycles=0; #number of cycles
    no_stall=0
    mem_str=0
    mem_data=0
    synchro=0
    comp_str=0
    comp_data=0
    control=0
    idle=0
    other=0

    reading_cycle = False
    cycle_stall = Stall.No_stall
    for line in inf:
        line = line.split(' ')
        if(line[0]=='CYCLE'):
            cycles=cycles+1
            reading_cycle = True
            continue
        
        if(line[0]=='SCHEDULER' and not ('0' in line[1])):
            reading_cycle = False
            continue

        if(line[0]=='SID' and not ('0' in line[1])):
            reading_cycle = False
            continue

        if reading_cycle and line[0] == 'warp':
            # Algorithm 1 Instruction Stall Classification
            warp_stall = Stall.No_stall
            if '1' in line[Stall.Comp_str]:
                warp_stall = Stall.Comp_str
            if '1' in line[Stall.Comp_data]:
                warp_stall = Stall.Comp_data
            if '1' in line[Stall.Mem_str]:
                warp_stall = Stall.Mem_str
            if '1' in line[Stall.Mem_data]:
                warp_stall = Stall.Mem_data
            if '1' in line[Stall.Synco]:
                warp_stall = Stall.Synco
            if '1' in line[Stall.Control]:
                warp_stall = Stall.Control
            if '1' in line[Stall.Idle]:
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
            if cycle_stall == Stall.Idle or warp_stall == Stall.Idle:
                cycle_stall = Stall.Idle
                continue


        if reading_cycle and line[1] == 'dispatched':
            reading_cycle = False
            if '0' in line[2]:
                if cycle_stall == Stall.Mem_str:
                    mem_str += 1
                    continue
                if cycle_stall == Stall.Mem_data:
                    mem_data += 1
                    continue
                if cycle_stall == Stall.Synco:
                    synchro += 1
                    continue
                if cycle_stall == Stall.Comp_str:
                    comp_str += 1
                    continue
                if cycle_stall == Stall.Comp_data:
                    comp_data += 1
                    continue
                if cycle_stall == Stall.Control:
                    control += 1
                    continue
                if cycle_stall == Stall.Idle:
                    idle += 1
                    continue
                other+=1

            else:
                no_stall += 1
                continue         

    #percentages of the stalls
    per_mem_str=mem_str/cycles*100
    per_mem_data=mem_data/cycles*100
    per_synchro=synchro/cycles*100
    per_comp_str=comp_str/cycles*100
    per_comp_data=comp_data/cycles*100
    per_control=control/cycles*100
    per_idle=idle/cycles*100
    per_other=other/cycles*100

    #speedup without the stall
    speed_mem_str=cycles/(cycles-mem_str)
    speed_mem_data=cycles/(cycles-mem_data)
    speed_synchro=cycles/(cycles-synchro)
    speed_comp_str=cycles/(cycles-comp_str)
    speed_comp_data=cycles/(cycles-comp_data)
    speed_control=cycles/(cycles-control)
    speed_idle=cycles/(cycles-idle)
    speed_other=cycles/(cycles-other)

    outf.write("CALCULATION METHOD\n")
    outf.write("Percetage stalls wrt total cycles: stall_type/total_cycles*100\n")
    outf.write("Speedup without stall: total_cycles/(total_cycles-stall_type)\n")
    outf.write("\n\n\n")
    outf.write("Total Cycles " +str(cycles)+"\n")
    outf.write("*********memory structural stall********\n")
    outf.write("Number of stalls:"+str(mem_str)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_mem_str)+"\n")
    outf.write("Speedup without stall:"+str(speed_mem_str)+"\n")
    outf.write("*********memory data stall********\n")
    outf.write("Number of stalls:"+str(mem_data)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_mem_data)+"\n")
    outf.write("Speedup without stall:"+str(speed_mem_data)+"\n")
    outf.write("*********synchronization stall********\n")
    outf.write("Number of stalls:"+str(synchro)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_synchro)+"\n")
    outf.write("Speedup without stall:"+str(speed_synchro)+"\n")
    outf.write("*********compute structural stall********\n")
    outf.write("Number of stalls:"+str(comp_str)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_comp_str)+"\n")
    outf.write("Speedup without stall:"+str(speed_comp_str)+"\n")
    outf.write("*********compute data stall********\n")
    outf.write("Number of stalls:"+str(comp_data)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_comp_data)+"\n")
    outf.write("Speedup without stall:"+str(speed_comp_data)+"\n")
    outf.write("*********compute control stall********\n")
    outf.write("Number of stalls:"+str(control)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_control)+"\n")
    outf.write("Speedup without stall:"+str(speed_control)+"\n")
    outf.write("*********idle stall********\n")
    outf.write("Number of stalls:"+str(idle)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_idle)+"\n")
    outf.write("Speedup without stall:"+str(speed_idle)+"\n")
    outf.write("*********other stall********\n")
    outf.write("Number of stalls:"+str(other)+"\n")
    outf.write("Percetage stalls wrt total cycles:"+str(per_other)+"\n")
    outf.write("Speedup without stall:"+str(speed_other)+"\n")


   

def main():
    filename="stall_output.txt"
    outfile="srad_v1_stats"
    fin=open(filename,"r")
    fout=open(outfile,"w")

    getstats(fin,fout)



if __name__ == "__main__":
    main()
