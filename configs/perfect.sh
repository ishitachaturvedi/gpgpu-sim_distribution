#!/bin/bash

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/cnfig_pascal_islip.icnt ./cnfig_pascal_islip.icnt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/gpgpusim.config ./gpgpusim.config
./$1 > normal.txt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/CU_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > CU_Perfect.txt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Exec_Perfect.txt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > Mem_Perfect.txt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/OC_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > OC_Perfect.txt

cp -f /u/ls24/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/ICache_Perfect/gpgpusim.config ./gpgpusim.config
./$1 > ICache_Perfect.txt
