#!/bin/bash

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/config_pascal_islip.icnt ./config_pascal_islip.icnt

echo NORMAL
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/gpgpusim.config ./gpgpusim.config
./$1 > normal.txt

echo EXEC_STRUCT
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Struct_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Struct_Perfect.txt

echo EXEC_STRUCT_OC
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Struct_OC_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Struct_OC_Perfect.txt

echo EXEC
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Perfect.txt

echo MEM
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Perfect.txt

echo L1
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/L1_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > L1_Perfect.txt

echo L2
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/L2_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > L2_Perfect.txt

echo MEM_STRUCT
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Struct_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Struct_Perfect.txt

echo MEM_STRUCT_OC
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Struct_OC_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Struct_OC_Perfect.txt

echo OC
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/OC_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > OC_Perfect.txt

echo ICache
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/ICache_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > ICache_Perfect.txt
