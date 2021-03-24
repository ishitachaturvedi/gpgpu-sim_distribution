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
    for line in inf:
        line = line.split(' ')
        if(line[0]=='cycle'):
            cycles=cycles+1
            if("0" in line[4]):
                no_stall=no_stall+1
            if("1" in line[4]):
                mem_str=mem_str+1
            if("2" in line[4]):
                mem_data=mem_data+1
            if("3" in line[4]):
                synchro=synchro+1
            if("4" in line[4]):
                comp_str=comp_str+1
            if("5" in line[4]):
                comp_data=comp_data+1
            if("6" in line[4]):
                control=control+1
            if("7" in line[4]):
                idle=idle+1

    #percentages of the stalls
    per_mem_str=mem_str/cycles*100
    per_mem_data=mem_data/cycles*100
    per_synchro=synchro/cycles*100
    per_comp_str=comp_str/cycles*100
    per_comp_data=comp_data/cycles*100
    per_control=control/cycles*100
    per_idle=idle/cycles*100

    #speedup without the stall
    speed_mem_str=cycles/(cycles-mem_str)
    speed_mem_data=cycles/(cycles-mem_data)
    speed_synchro=cycles/(cycles-synchro)
    speed_comp_str=cycles/(cycles-comp_str)
    speed_comp_data=cycles/(cycles-comp_data)
    speed_control=cycles/(cycles-control)
    speed_idle=cycles/(cycles-idle)

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


   

def main():
    filename="srad_v1_gsi"
    outfile="srad_v1_stats"
    fin=open(filename,"r")
    fout=open(outfile,"w")

    getstats(fin,fout)



if __name__ == "__main__":

    main()
