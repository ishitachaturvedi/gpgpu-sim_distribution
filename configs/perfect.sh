#!/bin/bash

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/config_pascal_islip.icnt ./config_pascal_islip.icnt

echo NORMAL
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/gpgpusim.config ./gpgpusim.config
./$1 > normal.txt

echo EXEC_STRUCT
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Struct_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Struct_Perfect.txt

echo EXEC
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Perfect.txt

echo MEM
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Perfect.txt

echo MEM_STRUCT
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Struct_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Struct_Perfect.txt

echo ICACHE
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/ICache_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > ICache_Perfect.txt

echo CONTROL
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Control_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Control_Perfect.txt

echo SYNCHRO
cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Synchro_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Synchro_Perfect.txt
