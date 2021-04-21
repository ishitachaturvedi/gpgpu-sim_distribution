from copy import deepcopy

#STALL ORDERING:
#mem_str = 0;
#mem_data = 1;
#synco = 2;
#comp_str = 3;
#comp_data = 4;
#control = 5;
#ibufferw = 6;
#imisspendingw = 7;
#pendingWritew = 8;

#sort function
def Sort(sub_li,key): 
    sub_li.sort(key = lambda x: x[key],reverse=True) 
    return sub_li 

def sorter(numStalls,warp,index):
    Sort(warp,index)
    return warp

#partition speedup calculator
def getSpeedup(warp_store,tempw,shader_count_temp,shader_count,indep,partition_cycle,numStalls,sid_range,warp_indep_c):
    #first do a comparison for all active warps for defining speeup
    indep_temp=deepcopy(indep)
    for i in range(numStalls):
        indep_temp[i][1]=-1
    indep_temp_per=deepcopy(indep)
    for i in range(numStalls):
        indep_temp_per[i][1]=-1
    #get min speedup per stall
    for sid in range(sid_range):
        for wid in warp_store:
            if(shader_count[sid][wid]!=-1):
                for i in range(numStalls):
                    speedup=partition_cycle/(partition_cycle-warp_indep_c[sid][wid][i][1]+1)
                    if(indep_temp[i][1]==-1):
                        indep_temp[i][1]=warp_indep_c[sid][wid][i][1]
                        indep_temp_per[i][1]=speedup
                        if(speedup<indep_temp_per[i][1]):
                            indep_temp[i][1]=warp_indep_c[sid][wid][i][1]
                            indep_temp_per[i][1]=speedup
            for i in range(numStalls):
                warp_indep_c[sid][wid][i][1]=0
    #add all the min stalls found to indep
    for i in range(numStalls):
        #if(i==1 and indep_temp[i][1]>0):
        #    print("enter partition cycle ",partition_cycle," data ",indep_temp[i]," init ",indep[i])
        indep[i][1]=indep[i][1]+indep_temp[i][1]
        #if(i==1 and indep_temp[i][1]>0):
        #    print("exit partition cycle ",partition_cycle," data ",indep_temp[i]," final ",indep[i])
    #make required values zero for next partition
    if(warp_store!=tempw):
        warp_store=deepcopy(tempw)
    if(shader_count!=shader_count_temp):
        shader_count=deepcopy(shader_count_temp)
    partition_cycle=0
    return indep,warp_store,shader_count,partition_cycle,warp_indep_c

#keep adding the stalls per cycle normally
def sumall(warp_indep_temp,warp_indep_c,numStalls,sid_range,warp_range):
    for sid in range(sid_range):
        for wid in range(warp_range):
            for i in range(numStalls):
                warp_indep_c[sid][wid][i][1]=warp_indep_c[sid][wid][i][1]+warp_indep_temp[sid][wid][i][1]
                warp_indep_temp[sid][wid][i][1]=0
    return warp_indep_c

#parse the text file and log data
def storeValues(sid_range,warp_range,warp_indep_c,filename,numStalls,shader_count,cycle,test,cycle_count,indep,warp_indep_temp):
    fin1=open(filename,"r")
    warp_store=[0]
    
    shader_count_init=deepcopy(shader_count)
    checker=deepcopy(shader_count)
    partition_cycle=0
    sid_start=0
    sid_active=0
    warp_active=0

    for line in fin1:
        line1=line
        line=line.split(' ')
        #add cycles
        if(line[0]=='Active'):
            cycle=cycle+1
            if(sid_active>0):
                #store all values and start a new partition
                if(warp_store!=tempw or shader_count_temp!=shader_count):
                    indep,warp_store,shader_count,partition_cycle,warp_indep_c=getSpeedup(warp_store,tempw,shader_count_temp,shader_count,indep,partition_cycle,numStalls,sid_range,warp_indep_c)
            if(sid_start==1):
                warp_indep_c=sumall(warp_indep_temp,warp_indep_c,numStalls,sid_range,warp_range)
            tempw=[]
            shader_count_temp=deepcopy(shader_count_init)
            sid_active=0
            warp_active=0
            partition_cycle=partition_cycle+1
        #begin start of sm
        if(line[0]=='SID'):
            sid=int(line[1])
            sid_start=1
            if(sid_active<sid):
                sid_active=sid
        #sort data per SM per warp into stacks
        if(line[0]=="warp"):
            warp_num=int(line[1])
            if(warp_active<warp_num):
                warp_active=warp_num
            tempw.append(warp_num)
            shader_count[sid][warp_num]=1
            t=0
            #if block is over--> number of active warps changes, find min speedup per stall and add it to indep
            for i in range(numStalls):
                if((line[(i+1)*4])=='1'):
                    t=1
                    warp_indep_temp[sid][warp_num][i][1]=1
                elif((line[(i+1)*4])=='2'):
                    t=1
            if(t==1):
                shader_count_temp[sid][warp_num]=1
                if(partition_cycle==1):
                    shader_count[sid][warp_num]=1

    return  warp_indep_c,cycle,test,cycle_count,indep

def init(warp_indep_c,warp_indep_temp,shader_count,test,cycle_count,sid_range,warp_range,numStalls):
    for i in range(sid_range):
        warp_indep_c_sid=[]
        warp_indep_c_sid1=[]
        warp_nine_c_sid=[]
        warp_nine_c_sid1=[]
        shader_count_sid=[]
        for j in range(warp_range):
            warp_indep_c_in=[]
            warp_indep_c_in1=[]
            warp_nine_c_sid.append(0)
            warp_nine_c_sid1.append(0)
            shader_count_sid.append(-1)
            for i1 in range(numStalls):
                temp=[]
                temp.append(i1)
                temp1=deepcopy(temp)
                temp1.append(0)
                temp2=deepcopy(temp1)
                warp_indep_c_in.append(temp1)
                warp_indep_c_in1.append(temp2)
            warp_indep_c_sid.append(warp_indep_c_in)
            warp_indep_c_sid1.append(warp_indep_c_in1)
        warp_indep_c.append(warp_indep_c_sid)
        warp_indep_temp.append(warp_indep_c_sid1)
        shader_count.append(shader_count_sid)
        test.append(warp_nine_c_sid)
        cycle_count.append(warp_nine_c_sid1)

    return warp_indep_c,warp_indep_temp,shader_count,test,cycle_count

#Read file line by line
def FileRead(filename,outfile,indep,numStalls,fout,cycle,indep_count):
    warp_indep_c=[]
    shader_count=[]
    warp_indep_temp=[]
    sid_range=30
    warp_range=64
    test=[]
    cycle_count=[]

    #initialise data structures to store values
    warp_indep_c,warp_indep_temp,shader_count,test,cycle_count=init(warp_indep_c,warp_indep_temp,shader_count,test,cycle_count,sid_range,warp_range,numStalls)

    #store data based on stalls
    warp_indep_c,cycle,test,cycle_count,indep=storeValues(sid_range,warp_range,warp_indep_c,filename,numStalls,shader_count,cycle,test,cycle_count,indep,warp_indep_temp)

    #sort the arrays
    indep=sorter(numStalls,indep,1)
    
    return indep,cycle,indep_count

def init1(indep,numStalls):
    for i1 in range(numStalls):
        temp=[]
        temp.append(i1)
        temp.append(0)
        indep.append(temp)

    return indep

#print the output
def printOut(indep,fout,cycle,indep_count):

    fout.write("TOTAL CYCLES "+str(cycle))
    names=['mem_str','mem_data','synco','comp_str','comp_data','control','ibufferw','imisspendingw','pendingWritew']
    #independent stalls
    fout.write("\n********* INDEPENDENT STALLS ************\n")
    num=0
    for i in range(len(indep)):
        if(indep[i][1]!=0):
            num=1
            speedup=cycle/(cycle-indep[i][1])
            if(round(speedup,8)>1):
                #fout.write(str(names[indep[i][0]])+" --> speedup : "+str(indep[i][1])+"\n")
                fout.write(str(names[indep[i][0]])+" --> count : "+str(indep[i][1])+" speedup % "+str(speedup)+"\n")
        if(indep[i][1]==0):
            break
    if(num==0):
        fout.write("No independent stalls\n")

    fout.write("\n")

def smallerFile(filename,fout):
    fin=open(filename,"r")
    c=0
    for line in fin:
        fout.write(line)
        c=c+1
        if(c>90000 and ("******" in line)):
            break

def main():
    filename="lavaMD_fast"
    outfile="lavaMD_fast_partition"
    fout=open(outfile,"w")
    numStalls=9 #Number of stalls to consider
    indep=[]
    indep_count=[]
    cycle=0
    
    
    indep=init1(indep,numStalls)
    indep_count=init1(indep_count,numStalls)
    #smallerFile(filename,fout)

    #Setup values for each 
    indep,cycle,indep_count=FileRead(filename,outfile,indep,numStalls,fout,cycle,indep_count)

    #print the output
    printOut(indep,fout,cycle,indep_count)

if __name__ == "__main__":

    main()
