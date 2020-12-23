// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ivan Sham,
// Andrew Turner, Ali Bakhoda, The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "gpgpusim_entrypoint.h"
#include <stdio.h>

#include "../libcuda/gpgpu_context.h"
#include "cuda-sim/cuda-sim.h"
#include "cuda-sim/ptx_ir.h"
#include "cuda-sim/ptx_parser.h"
#include "gpgpu-sim/gpu-sim.h"
#include "gpgpu-sim/icnt_wrapper.h"
#include "option_parser.h"
#include "stream_manager.h"
#include "gpgpu-sim/gsi_prof.h"
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#include<vector>
using std::vector;

//give initial size for number of cycles to be considered
int initSize=50000;
int cycle_num=0;
//initialise vector of vectors as stallData[Cycle #][Warp #]
vector<int> stallData;
//define temp array to store data
vector<int> tempw;
//if warp active corresponding value set to 1
vector<int> activew;
int active_warp;
int idlew=0;
//give priority to different stalls
//0-max priority
//10-least priority
////change assigned values to flip level of importance
////to make any check inactive change valye of check to > initial value of temp
int mem_str = 4;
int mem_data = 3;
int synco = 2;
int comp_str = 6;
int comp_data = 5;
int control = 1;
int idle = 0;
int ibufferw = 100;
int imisspendingw = 100;
int ocpending = 100;
int globalw = 100;
int localw = 100;
//variables for bucketing
int mem_str_c = 0;
int mem_data_c = 0;
int synco_c = 0;
int comp_str_c = 0;
int comp_data_c = 0;
int control_c = 0;
int idle_c = 0;
int ibuffer_c = 0;
int imisspending_c = 0;
int ocpending_c = 0;
int globalw_c = 0;
int localw_c = 0;
static int sg_argc = 3;
static const char *sg_argv[] = {"", "-config", "gpgpusim.config"};

void *gpgpu_sim_thread_sequential(void *ctx_ptr) {
  gpgpu_context *ctx = (gpgpu_context *)ctx_ptr;
  // at most one kernel running at a time
  bool done;
  do {
    sem_wait(&(ctx->the_gpgpusim->g_sim_signal_start));
    done = true;
    if (ctx->the_gpgpusim->g_the_gpu->get_more_cta_left()) {
      done = false;
      ctx->the_gpgpusim->g_the_gpu->init();
      while (ctx->the_gpgpusim->g_the_gpu->active()) {
        ctx->the_gpgpusim->g_the_gpu->cycle();
        ctx->the_gpgpusim->g_the_gpu->deadlock_check();
      }
      ctx->the_gpgpusim->g_the_gpu->print_stats();
      ctx->the_gpgpusim->g_the_gpu->update_stats();
      ctx->print_simulation_time();
    }
    sem_post(&(ctx->the_gpgpusim->g_sim_signal_finish));
  } while (!done);
  sem_post(&(ctx->the_gpgpusim->g_sim_signal_exit));
  return NULL;
}

static void termination_callback() {
  printf("The STALLS are\n0-no stall\n1-mem_str\n2-mem_data\n3-synchronization\n4-comp_str\n5-comp_data\n6-control stall\n7-idle stall\n");
  printf("GPGPU-Sim: *** exit detected ***\n");
  fflush(stdout);
}

void *gpgpu_sim_thread_concurrent(void *ctx_ptr) {
  gpgpu_context *ctx = (gpgpu_context *)ctx_ptr;
  atexit(termination_callback);
  //appropriately sizing arrays for use
  initSize=50000;
  stallData.resize(32,10);
  tempw.resize(32,10);
  activew.resize(32,0);
  // concurrent kernel execution simulation thread
  do {
    if (g_debug_execution >= 3) {
      printf(
          "GPGPU-Sim: *** simulation thread starting and spinning waiting for "
          "work ***\n");
      fflush(stdout);
    }
    while (ctx->the_gpgpusim->g_stream_manager->empty_protected() &&
           !ctx->the_gpgpusim->g_sim_done)
      ;
    if (g_debug_execution >= 3) {
      printf("GPGPU-Sim: ** START simulation thread (detected work) **\n");
      ctx->the_gpgpusim->g_stream_manager->print(stdout);
      fflush(stdout);
    }
    pthread_mutex_lock(&(ctx->the_gpgpusim->g_sim_lock));
    ctx->the_gpgpusim->g_sim_active = true;
    pthread_mutex_unlock(&(ctx->the_gpgpusim->g_sim_lock));
    bool active = false;
    bool sim_cycles = false;
    ctx->the_gpgpusim->g_the_gpu->init();
    do {
      // check if a kernel has completed
      // launch operation on device if one is pending and can be run

      // Need to break this loop when a kernel completes. This was a
      // source of non-deterministic behaviour in GPGPU-Sim (bug 147).
      // If another stream operation is available, g_the_gpu remains active,
      // causing this loop to not break. If the next operation happens to be
      // another kernel, the gpu is not re-initialized and the inter-kernel
      // behaviour may be incorrect. Check that a kernel has finished and
      // no other kernel is currently running.
      if (ctx->the_gpgpusim->g_stream_manager->operation(&sim_cycles) &&
          !ctx->the_gpgpusim->g_the_gpu->active())
        break;

      // functional simulation
      if (ctx->the_gpgpusim->g_the_gpu->is_functional_sim()) {
        kernel_info_t *kernel =
            ctx->the_gpgpusim->g_the_gpu->get_functional_kernel();
        assert(kernel);
        ctx->the_gpgpusim->gpgpu_ctx->func_sim->gpgpu_cuda_ptx_sim_main_func(
            *kernel);
        ctx->the_gpgpusim->g_the_gpu->finish_functional_sim(kernel);
      }

      // performance simulation
      if (ctx->the_gpgpusim->g_the_gpu->active()) {
        ctx->the_gpgpusim->g_the_gpu->cycle();
        sim_cycles = true;
        ctx->the_gpgpusim->g_the_gpu->deadlock_check();
      } else {
        if (ctx->the_gpgpusim->g_the_gpu->cycle_insn_cta_max_hit()) {
          ctx->the_gpgpusim->g_stream_manager->stop_all_running_kernels();
          ctx->the_gpgpusim->g_sim_done = true;
          ctx->the_gpgpusim->break_limit = true;
        }
      }

      active = ctx->the_gpgpusim->g_the_gpu->active() ||
               !(ctx->the_gpgpusim->g_stream_manager->empty_protected());

    } while (active && !ctx->the_gpgpusim->g_sim_done);
    if (g_debug_execution >= 3) {
      printf("GPGPU-Sim: ** STOP simulation thread (no work) **\n");
      fflush(stdout);
    }
    if (sim_cycles) {
      ctx->the_gpgpusim->g_the_gpu->print_stats();
      ctx->the_gpgpusim->g_the_gpu->update_stats();
      ctx->print_simulation_time();
    }
    pthread_mutex_lock(&(ctx->the_gpgpusim->g_sim_lock));
    ctx->the_gpgpusim->g_sim_active = false;
    pthread_mutex_unlock(&(ctx->the_gpgpusim->g_sim_lock));
  } while (!ctx->the_gpgpusim->g_sim_done);

  printf("GPGPU-Sim: *** simulation thread exiting ***\n");
  fflush(stdout);

  if (ctx->the_gpgpusim->break_limit) {
    printf(
        "GPGPU-Sim: ** break due to reaching the maximum cycles (or "
        "instructions) **\n");
    exit(1);
  }

  sem_post(&(ctx->the_gpgpusim->g_sim_signal_exit));
  return NULL;
}

void gpgpu_context::synchronize() {
  printf("GPGPU-Sim: synchronize waiting for inactive GPU simulation\n");
  the_gpgpusim->g_stream_manager->print(stdout);
  fflush(stdout);
  //    sem_wait(&g_sim_signal_finish);
  bool done = false;
  do {
    pthread_mutex_lock(&(the_gpgpusim->g_sim_lock));
    done = (the_gpgpusim->g_stream_manager->empty() &&
            !the_gpgpusim->g_sim_active) ||
           the_gpgpusim->g_sim_done;
    pthread_mutex_unlock(&(the_gpgpusim->g_sim_lock));
  } while (!done);
  printf("GPGPU-Sim: detected inactive GPU simulation thread\n");
  fflush(stdout);
  //    sem_post(&g_sim_signal_start);
}

void gpgpu_context::exit_simulation() {
  the_gpgpusim->g_sim_done = true;
  printf("GPGPU-Sim: exit_simulation called\n");
  fflush(stdout);
  sem_wait(&(the_gpgpusim->g_sim_signal_exit));
  printf("GPGPU-Sim: simulation thread signaled exit\n");
  fflush(stdout);
}

gpgpu_sim *gpgpu_context::gpgpu_ptx_sim_init_perf() {
  srand(1);
  print_splash();
  func_sim->read_sim_environment_variables();
  ptx_parser->read_parser_environment_variables();
  option_parser_t opp = option_parser_create();

  ptx_reg_options(opp);
  func_sim->ptx_opcocde_latency_options(opp);

  icnt_reg_options(opp);
  the_gpgpusim->g_the_gpu_config = new gpgpu_sim_config(this);
  the_gpgpusim->g_the_gpu_config->reg_options(
      opp);  // register GPU microrachitecture options

  option_parser_cmdline(opp, sg_argc, sg_argv);  // parse configuration options
  fprintf(stdout, "GPGPU-Sim: Configuration options:\n\n");
  option_parser_print(opp, stdout);
  // Set the Numeric locale to a standard locale where a decimal point is a
  // "dot" not a "comma" so it does the parsing correctly independent of the
  // system environment variables
  assert(setlocale(LC_NUMERIC, "C"));
  the_gpgpusim->g_the_gpu_config->init();

  the_gpgpusim->g_the_gpu =
      new exec_gpgpu_sim(*(the_gpgpusim->g_the_gpu_config), this);
  the_gpgpusim->g_stream_manager = new stream_manager(
      (the_gpgpusim->g_the_gpu), func_sim->g_cuda_launch_blocking);

  the_gpgpusim->g_simulation_starttime = time((time_t *)NULL);

  sem_init(&(the_gpgpusim->g_sim_signal_start), 0, 0);
  sem_init(&(the_gpgpusim->g_sim_signal_finish), 0, 0);
  sem_init(&(the_gpgpusim->g_sim_signal_exit), 0, 0);

  return the_gpgpusim->g_the_gpu;
}

void gpgpu_context::start_sim_thread(int api) {
  if (the_gpgpusim->g_sim_done) {
    the_gpgpusim->g_sim_done = false;
    if (api == 1) {
      pthread_create(&(the_gpgpusim->g_simulation_thread), NULL,
                     gpgpu_sim_thread_concurrent, (void *)this);
    } else {
      pthread_create(&(the_gpgpusim->g_simulation_thread), NULL,
                     gpgpu_sim_thread_sequential, (void *)this);
    }
  }
}

void gpgpu_context::print_simulation_time() {
  time_t current_time, difference, d, h, m, s;
  current_time = time((time_t *)NULL);
  difference = MAX(current_time - the_gpgpusim->g_simulation_starttime, 1);

  d = difference / (3600 * 24);
  h = difference / 3600 - 24 * d;
  m = difference / 60 - 60 * (h + 24 * d);
  s = difference - 60 * (m + 60 * (h + 24 * d));

  fflush(stderr);
  printf(
      "\n\ngpgpu_simulation_time = %u days, %u hrs, %u min, %u sec (%u sec)\n",
      (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)s, (unsigned)difference);
  printf("gpgpu_simulation_rate = %u (inst/sec)\n",
         (unsigned)(the_gpgpusim->g_the_gpu->gpu_tot_sim_insn / difference));
  const unsigned cycles_per_sec =
      (unsigned)(the_gpgpusim->g_the_gpu->gpu_tot_sim_cycle / difference);
  printf("gpgpu_simulation_rate = %u (cycle/sec)\n", cycles_per_sec);
  printf("gpgpu_silicon_slowdown = %ux\n",
         the_gpgpusim->g_the_gpu->shader_clock() * 1000 / cycles_per_sec);
  fflush(stdout);
}

int gpgpu_context::gpgpu_opencl_ptx_sim_main_perf(kernel_info_t *grid) {
  the_gpgpusim->g_the_gpu->launch(grid);
  sem_post(&(the_gpgpusim->g_sim_signal_start));
  sem_wait(&(the_gpgpusim->g_sim_signal_finish));
  return 0;
}

//! Functional simulation of OpenCL
/*!
 * This function call the CUDA PTX functional simulator
 */
int cuda_sim::gpgpu_opencl_ptx_sim_main_func(kernel_info_t *grid) {
  // calling the CUDA PTX simulator, sending the kernel by reference and a flag
  // set to true, the flag used by the function to distinguish OpenCL calls from
  // the CUDA simulation calls which it is needed by the called function to not
  // register the exit the exit of OpenCL kernel as it doesn't register entering
  // in the first place as the CUDA kernels does
  gpgpu_cuda_ptx_sim_main_func(*grid, true);
  return 0;
}
