#!/bin/bash

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/config_pascal_islip.icnt ./config_pascal_islip.icnt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/gpgpusim.config ./gpgpusim.config
./run > normal.txt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/CU_Perfect/gpgpusim.config ./gpgpusim.config
./run > CU_Perfect.txt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Exec_Perfect/gpgpusim.config ./gpgpusim.config
./run > Exec_Perfect.txt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/Mem_Perfect/gpgpusim.config ./gpgpusim.config
./run > Mem_Perfect.txt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/OC_Perfect/gpgpusim.config ./gpgpusim.config
./run > OC_Perfect.txt

cp -f /u/ishitac/gpgpusim-codes/perfect/gpgpu-sim_distribution/configs/tested-cfgs/SM6_TITANX/ICache_Perfect/gpgpusim.config ./gpgpusim.config
./run > ICache_Perfect.txt
