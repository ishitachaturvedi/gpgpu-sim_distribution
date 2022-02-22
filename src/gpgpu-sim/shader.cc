// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ali Bakhoda,
// George L. Yuan, Andrew Turner, Inderpreet Singh
// The University of British Columbia
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

#include "shader.h"
#include <float.h>
#include <limits.h>
#include <string.h>
#include "../../libcuda/gpgpu_context.h"
#include "../cuda-sim/cuda-sim.h"
#include "../cuda-sim/ptx-stats.h"
#include "../cuda-sim/ptx_sim.h"
#include "../statwrapper.h"
#include "addrdec.h"
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "icnt_wrapper.h"
#include "mem_fetch.h"
#include "mem_latency_stat.h"
#include "shader_trace.h"
#include "stat-tool.h"
#include "traffic_breakdown.h"
#include "visualizer.h"
#include "fast.h"
#include <iostream>

using namespace std;

typedef enum {
    mem_str = 0,
    mem_data,
    synco,
    comp_str,
    comp_data,
    control,
    ibufferw,
    imisspendingw,
    pendingWritew,
    idlew,
    others,
    reserve_mem,
    release_mem,
    reserve_comp,
    release_comp,
    OP_TYPE, //check inst type -> Look at Struct_stall_types
    block_id
} StallReasons;

typedef enum {
  issue,
  issueWarp
} issueReason;

typedef enum {
    mem_inst = 0,
    sp_inst,
    sfu_inst,
    tensor_inst,
    dp_inst,
    int_inst,
    spec_inst,
    alu_sfu,
    barrier_op,
    barrier_mem
} Struct_stall_types;

#define PRIORITIZE_MSHR_OVER_WB 1
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

mem_fetch *shader_core_mem_fetch_allocator::alloc(
    new_addr_type addr, mem_access_type type, unsigned size, bool wr,
    unsigned long long cycle) const {
  mem_access_t access(type, addr, size, wr, m_memory_config->gpgpu_ctx);
  mem_fetch *mf =
      new mem_fetch(access, NULL, wr ? WRITE_PACKET_SIZE : READ_PACKET_SIZE, -1,
                    m_core_id, m_cluster_id, m_memory_config, cycle);
  return mf;
}
/////////////////////////////////////////////////////////////////////////////

std::list<unsigned> shader_core_ctx::get_regs_written(const inst_t &fvt) const {
  std::list<unsigned> result;
  for (unsigned op = 0; op < MAX_REG_OPERANDS; op++) {
    int reg_num = fvt.arch_reg.dst[op];  // this math needs to match that used
                                         // in function_info::ptx_decode_inst
    if (reg_num >= 0)                    // valid register
      result.push_back(reg_num);
  }
  return result;
}

void exec_shader_core_ctx::create_shd_warp() {
  m_warp.resize(m_config->max_warps_per_shader);
  for (unsigned k = 0; k < m_config->max_warps_per_shader; ++k) {
    m_warp[k] = new shd_warp_t(this, m_config->warp_size);
  }
}

void shader_core_ctx::create_front_pipeline() {
  // pipeline_stages is the sum of normal pipeline stages and specialized_unit
  // stages * 2 (for ID and EX)
  unsigned total_pipeline_stages =
      N_PIPELINE_STAGES + m_config->m_specialized_unit.size() * 2;
  m_pipeline_reg.reserve(total_pipeline_stages);
  for (int j = 0; j < N_PIPELINE_STAGES; j++) {
    m_pipeline_reg.push_back(
        register_set(m_config->pipe_widths[j], pipeline_stage_name_decode[j]));
  }
  for (int j = 0; j < m_config->m_specialized_unit.size(); j++) {
    m_pipeline_reg.push_back(
        register_set(m_config->m_specialized_unit[j].id_oc_spec_reg_width,
                     m_config->m_specialized_unit[j].name));
    m_config->m_specialized_unit[j].ID_OC_SPEC_ID = m_pipeline_reg.size() - 1;
    m_specilized_dispatch_reg.push_back(
        &m_pipeline_reg[m_pipeline_reg.size() - 1]);
  }
  for (int j = 0; j < m_config->m_specialized_unit.size(); j++) {
    m_pipeline_reg.push_back(
        register_set(m_config->m_specialized_unit[j].oc_ex_spec_reg_width,
                     m_config->m_specialized_unit[j].name));
    m_config->m_specialized_unit[j].OC_EX_SPEC_ID = m_pipeline_reg.size() - 1;
  }

  if (m_config->sub_core_model) {
    // in subcore model, each scheduler should has its own issue register, so
    // num scheduler = reg width
    assert(m_config->gpgpu_num_sched_per_core ==
           m_pipeline_reg[ID_OC_SP].get_size());
    assert(m_config->gpgpu_num_sched_per_core ==
           m_pipeline_reg[ID_OC_SFU].get_size());
    assert(m_config->gpgpu_num_sched_per_core ==
           m_pipeline_reg[ID_OC_MEM].get_size());
    if (m_config->gpgpu_tensor_core_avail)
      assert(m_config->gpgpu_num_sched_per_core ==
             m_pipeline_reg[ID_OC_TENSOR_CORE].get_size());
    if (m_config->gpgpu_num_dp_units > 0)
      assert(m_config->gpgpu_num_sched_per_core ==
             m_pipeline_reg[ID_OC_DP].get_size());
    if (m_config->gpgpu_num_int_units > 0)
      assert(m_config->gpgpu_num_sched_per_core ==
             m_pipeline_reg[ID_OC_INT].get_size());
  }

  m_threadState = (thread_ctx_t *)calloc(sizeof(thread_ctx_t),
                                         m_config->n_thread_per_shader);

  m_not_completed = 0;
  m_active_threads.reset();
  m_n_active_cta = 0;
  for (unsigned i = 0; i < MAX_CTA_PER_SHADER; i++) m_cta_status[i] = 0;
  for (unsigned i = 0; i < m_config->n_thread_per_shader; i++) {
    m_thread[i] = NULL;
    m_threadState[i].m_cta_id = -1;
    m_threadState[i].m_active = false;
  }

  // m_icnt = new shader_memory_interface(this,cluster);
  if (m_config->gpgpu_perfect_mem) {
    m_icnt = new perfect_memory_interface(this, m_cluster);
  } else {
    m_icnt = new shader_memory_interface(this, m_cluster);
  }
  m_mem_fetch_allocator =
      new shader_core_mem_fetch_allocator(m_sid, m_tpc, m_memory_config);

  // fetch
  m_last_warp_fetched = 0;

#define STRSIZE 1024
  char name[STRSIZE];
  snprintf(name, STRSIZE, "L1I_%03d", m_sid);
  m_L1I = new read_only_cache(name, m_config->m_L1I_config, m_sid,
                              get_shader_instruction_cache_id(), m_icnt,
                              IN_L1I_MISS_QUEUE);
}

void shader_core_ctx::create_schedulers() {
  m_scoreboard = new Scoreboard(m_sid, m_config->max_warps_per_shader, m_gpu);

  // scedulers
  // must currently occur after all inputs have been initialized.
  std::string sched_config = m_config->gpgpu_scheduler_string;
  const concrete_scheduler scheduler =
        sched_config.find("fast") != std::string::npos
          ? CONCRETE_SCHEDULER_FAST
          : sched_config.find("lrr") != std::string::npos
              ? CONCRETE_SCHEDULER_LRR
              : sched_config.find("two_level_active") != std::string::npos
                    ? CONCRETE_SCHEDULER_TWO_LEVEL_ACTIVE
                    : sched_config.find("gto") != std::string::npos
                          ? CONCRETE_SCHEDULER_GTO
                          : sched_config.find("old") != std::string::npos
                                ? CONCRETE_SCHEDULER_OLDEST_FIRST
                                : sched_config.find("warp_limiting") !=
                                          std::string::npos
                                      ? CONCRETE_SCHEDULER_WARP_LIMITING
                                      : NUM_CONCRETE_SCHEDULERS;
  assert(scheduler != NUM_CONCRETE_SCHEDULERS);

  for (unsigned i = 0; i < m_config->gpgpu_num_sched_per_core; i++) {
    switch (scheduler) {
      case CONCRETE_SCHEDULER_FAST:
        schedulers.push_back(new fast_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i));
        break;
      case CONCRETE_SCHEDULER_LRR:
        schedulers.push_back(new lrr_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i));
        break;
      case CONCRETE_SCHEDULER_TWO_LEVEL_ACTIVE:
        schedulers.push_back(new two_level_active_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i, m_config->gpgpu_scheduler_string));
        break;
      case CONCRETE_SCHEDULER_GTO:
        schedulers.push_back(new gto_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i));
        break;
      case CONCRETE_SCHEDULER_OLDEST_FIRST:
        schedulers.push_back(new oldest_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i));
        break;
      case CONCRETE_SCHEDULER_WARP_LIMITING:
        schedulers.push_back(new swl_scheduler(
            m_stats, this, m_scoreboard, m_simt_stack, &m_warp,
            &m_pipeline_reg[ID_OC_SP], &m_pipeline_reg[ID_OC_DP],
            &m_pipeline_reg[ID_OC_SFU], &m_pipeline_reg[ID_OC_INT],
            &m_pipeline_reg[ID_OC_TENSOR_CORE], m_specilized_dispatch_reg,
            &m_pipeline_reg[ID_OC_MEM], i, m_config->gpgpu_scheduler_string));
        break;
      default:
        abort();
    };
  }

  for (unsigned i = 0; i < m_warp.size(); i++) {
    // distribute i's evenly though schedulers;
    schedulers[i % m_config->gpgpu_num_sched_per_core]->add_supervised_warp_id(
        i);
  }
  for (unsigned i = 0; i < m_config->gpgpu_num_sched_per_core; ++i) {
    schedulers[i]->done_adding_supervised_warps();
  }
}

void shader_core_ctx::create_exec_pipeline() {
  // op collector configuration
  enum { SP_CUS, DP_CUS, SFU_CUS, TENSOR_CORE_CUS, INT_CUS, MEM_CUS, GEN_CUS };

  opndcoll_rfu_t::port_vector_t in_ports;
  opndcoll_rfu_t::port_vector_t out_ports;
  opndcoll_rfu_t::uint_vector_t cu_sets;

  // configure generic collectors
  m_operand_collector.add_cu_set(
      GEN_CUS, m_config->gpgpu_operand_collector_num_units_gen,
      m_config->gpgpu_operand_collector_num_out_ports_gen);

  for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_gen;
       i++) {
    in_ports.push_back(&m_pipeline_reg[ID_OC_SP]);
    in_ports.push_back(&m_pipeline_reg[ID_OC_SFU]);
    in_ports.push_back(&m_pipeline_reg[ID_OC_MEM]);
    out_ports.push_back(&m_pipeline_reg[OC_EX_SP]);
    out_ports.push_back(&m_pipeline_reg[OC_EX_SFU]);
    out_ports.push_back(&m_pipeline_reg[OC_EX_MEM]);
    if (m_config->gpgpu_tensor_core_avail) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_TENSOR_CORE]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_TENSOR_CORE]);
    }
    if (m_config->gpgpu_num_dp_units > 0) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_DP]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_DP]);
    }
    if (m_config->gpgpu_num_int_units > 0) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_INT]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_INT]);
    }
    if (m_config->m_specialized_unit.size() > 0) {
      for (unsigned j = 0; j < m_config->m_specialized_unit.size(); ++j) {
        in_ports.push_back(
            &m_pipeline_reg[m_config->m_specialized_unit[j].ID_OC_SPEC_ID]);
        out_ports.push_back(
            &m_pipeline_reg[m_config->m_specialized_unit[j].OC_EX_SPEC_ID]);
      }
    }
    cu_sets.push_back((unsigned)GEN_CUS);
    m_operand_collector.add_port(in_ports, out_ports, cu_sets);
    in_ports.clear(), out_ports.clear(), cu_sets.clear();
  }

  if (m_config->enable_specialized_operand_collector) {
    m_operand_collector.add_cu_set(
        SP_CUS, m_config->gpgpu_operand_collector_num_units_sp,
        m_config->gpgpu_operand_collector_num_out_ports_sp);
    m_operand_collector.add_cu_set(
        DP_CUS, m_config->gpgpu_operand_collector_num_units_dp,
        m_config->gpgpu_operand_collector_num_out_ports_dp);
    m_operand_collector.add_cu_set(
        TENSOR_CORE_CUS,
        m_config->gpgpu_operand_collector_num_units_tensor_core,
        m_config->gpgpu_operand_collector_num_out_ports_tensor_core);
    m_operand_collector.add_cu_set(
        SFU_CUS, m_config->gpgpu_operand_collector_num_units_sfu,
        m_config->gpgpu_operand_collector_num_out_ports_sfu);
    m_operand_collector.add_cu_set(
        MEM_CUS, m_config->gpgpu_operand_collector_num_units_mem,
        m_config->gpgpu_operand_collector_num_out_ports_mem);
    m_operand_collector.add_cu_set(
        INT_CUS, m_config->gpgpu_operand_collector_num_units_int,
        m_config->gpgpu_operand_collector_num_out_ports_int);

    for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_sp;
         i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_SP]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_SP]);
      cu_sets.push_back((unsigned)SP_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }

    for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_dp;
         i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_DP]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_DP]);
      cu_sets.push_back((unsigned)DP_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }

    for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_sfu;
         i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_SFU]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_SFU]);
      cu_sets.push_back((unsigned)SFU_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }

    for (unsigned i = 0;
         i < m_config->gpgpu_operand_collector_num_in_ports_tensor_core; i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_TENSOR_CORE]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_TENSOR_CORE]);
      cu_sets.push_back((unsigned)TENSOR_CORE_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }

    for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_mem;
         i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_MEM]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_MEM]);
      cu_sets.push_back((unsigned)MEM_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }

    for (unsigned i = 0; i < m_config->gpgpu_operand_collector_num_in_ports_int;
         i++) {
      in_ports.push_back(&m_pipeline_reg[ID_OC_INT]);
      out_ports.push_back(&m_pipeline_reg[OC_EX_INT]);
      cu_sets.push_back((unsigned)INT_CUS);
      cu_sets.push_back((unsigned)GEN_CUS);
      m_operand_collector.add_port(in_ports, out_ports, cu_sets);
      in_ports.clear(), out_ports.clear(), cu_sets.clear();
    }
  }

  m_operand_collector.init(m_config->gpgpu_num_reg_banks, this);

  m_num_function_units =
      m_config->gpgpu_num_sp_units + m_config->gpgpu_num_dp_units +
      m_config->gpgpu_num_sfu_units + m_config->gpgpu_num_tensor_core_units +
      m_config->gpgpu_num_int_units + m_config->m_specialized_unit_num +
      1;  // sp_unit, sfu, dp, tensor, int, ldst_unit
  // m_dispatch_port = new enum pipeline_stage_name_t[ m_num_function_units ];
  // m_issue_port = new enum pipeline_stage_name_t[ m_num_function_units ];

  // m_fu = new simd_function_unit*[m_num_function_units];

  for (int k = 0; k < m_config->gpgpu_num_sp_units; k++) {
    m_fu.push_back(new sp_unit(&m_pipeline_reg[EX_WB], m_config, this));
    m_dispatch_port.push_back(ID_OC_SP);
    m_issue_port.push_back(OC_EX_SP);
  }

  for (int k = 0; k < m_config->gpgpu_num_dp_units; k++) {
    m_fu.push_back(new dp_unit(&m_pipeline_reg[EX_WB], m_config, this));
    m_dispatch_port.push_back(ID_OC_DP);
    m_issue_port.push_back(OC_EX_DP);
  }
  for (int k = 0; k < m_config->gpgpu_num_int_units; k++) {
    m_fu.push_back(new int_unit(&m_pipeline_reg[EX_WB], m_config, this));
    m_dispatch_port.push_back(ID_OC_INT);
    m_issue_port.push_back(OC_EX_INT);
  }

  for (int k = 0; k < m_config->gpgpu_num_sfu_units; k++) {
    m_fu.push_back(new sfu(&m_pipeline_reg[EX_WB], m_config, this));
    m_dispatch_port.push_back(ID_OC_SFU);
    m_issue_port.push_back(OC_EX_SFU);
  }

  for (int k = 0; k < m_config->gpgpu_num_tensor_core_units; k++) {
    m_fu.push_back(new tensor_core(&m_pipeline_reg[EX_WB], m_config, this));
    m_dispatch_port.push_back(ID_OC_TENSOR_CORE);
    m_issue_port.push_back(OC_EX_TENSOR_CORE);
  }

  for (int j = 0; j < m_config->m_specialized_unit.size(); j++) {
    for (unsigned k = 0; k < m_config->m_specialized_unit[j].num_units; k++) {
      m_fu.push_back(new specialized_unit(
          &m_pipeline_reg[EX_WB], m_config, this, SPEC_UNIT_START_ID + j,
          m_config->m_specialized_unit[j].name,
          m_config->m_specialized_unit[j].latency));
      m_dispatch_port.push_back(m_config->m_specialized_unit[j].ID_OC_SPEC_ID);
      m_issue_port.push_back(m_config->m_specialized_unit[j].OC_EX_SPEC_ID);
    }
  }

  m_ldst_unit = new ldst_unit(m_icnt, m_mem_fetch_allocator, this,
                              &m_operand_collector, m_scoreboard, m_config,
                              m_memory_config, m_stats, m_sid, m_tpc);
  m_fu.push_back(m_ldst_unit);
  m_dispatch_port.push_back(ID_OC_MEM);
  m_issue_port.push_back(OC_EX_MEM);

  assert(m_num_function_units == m_fu.size() and
         m_fu.size() == m_dispatch_port.size() and
         m_fu.size() == m_issue_port.size());

  // there are as many result buses as the width of the EX_WB stage
  num_result_bus = m_config->pipe_widths[EX_WB];
  for (unsigned i = 0; i < num_result_bus; i++) {
    this->m_result_bus.push_back(new std::bitset<MAX_ALU_LATENCY>());
  }
}

shader_core_ctx::shader_core_ctx(class gpgpu_sim *gpu,
                                 class simt_core_cluster *cluster,
                                 unsigned shader_id, unsigned tpc_id,
                                 const shader_core_config *config,
                                 const memory_config *mem_config,
                                 shader_core_stats *stats)
    : core_t(gpu, NULL, config->warp_size, config->n_thread_per_shader),
      m_barriers(this, config->max_warps_per_shader, config->max_cta_per_core,
                 config->max_barriers_per_cta, config->warp_size),
      m_active_warps(0),
      m_dynamic_warp_id(0) {
  m_cluster = cluster;
  m_config = config;
  m_memory_config = mem_config;
  m_stats = stats;
  unsigned warp_size = config->warp_size;
  Issue_Prio = 0;

  m_sid = shader_id;
  m_tpc = tpc_id;

  m_last_inst_gpu_sim_cycle = 0;
  m_last_inst_gpu_tot_sim_cycle = 0;

  // Jin: for concurrent kernels on a SM
  m_occupied_n_threads = 0;
  m_occupied_shmem = 0;
  m_occupied_regs = 0;
  m_occupied_ctas = 0;
  m_occupied_hwtid.reset();
  m_occupied_cta_to_hwtid.clear();
}

void shader_core_ctx::reinit(unsigned start_thread, unsigned end_thread,
                             bool reset_not_completed) {
  if (reset_not_completed) {
    m_not_completed = 0;
    m_active_threads.reset();

    // Jin: for concurrent kernels on a SM
    m_occupied_n_threads = 0;
    m_occupied_shmem = 0;
    m_occupied_regs = 0;
    m_occupied_ctas = 0;
    m_occupied_hwtid.reset();
    m_occupied_cta_to_hwtid.clear();
    m_active_warps = 0;
  }
  for (unsigned i = start_thread; i < end_thread; i++) {
    m_threadState[i].n_insn = 0;
    m_threadState[i].m_cta_id = -1;
  }
  for (unsigned i = start_thread / m_config->warp_size;
       i < end_thread / m_config->warp_size; ++i) {
    m_warp[i]->reset();
    m_simt_stack[i]->reset();
  }
}

void shader_core_ctx::init_warps(unsigned cta_id, unsigned start_thread,
                                 unsigned end_thread, unsigned ctaid,
                                 int cta_size, kernel_info_t &kernel) {
  //
  address_type start_pc = next_pc(start_thread);
  unsigned kernel_id = kernel.get_uid();
  if (m_config->model == POST_DOMINATOR) {
    unsigned start_warp = start_thread / m_config->warp_size;
    unsigned warp_per_cta = cta_size / m_config->warp_size;
    unsigned end_warp = end_thread / m_config->warp_size +
                        ((end_thread % m_config->warp_size) ? 1 : 0);
    for (unsigned i = start_warp; i < end_warp; ++i) {
      unsigned n_active = 0;
      simt_mask_t active_threads;
      for (unsigned t = 0; t < m_config->warp_size; t++) {
        unsigned hwtid = i * m_config->warp_size + t;
        if (hwtid < end_thread) {
          n_active++;
          assert(!m_active_threads.test(hwtid));
          m_active_threads.set(hwtid);
          active_threads.set(t);
        }
      }
      m_simt_stack[i]->launch(start_pc, active_threads);

      if (m_gpu->resume_option == 1 && kernel_id == m_gpu->resume_kernel &&
          ctaid >= m_gpu->resume_CTA && ctaid < m_gpu->checkpoint_CTA_t) {
        char fname[2048];
        snprintf(fname, 2048, "checkpoint_files/warp_%d_%d_simt.txt",
                 i % warp_per_cta, ctaid);
        unsigned pc, rpc;
        m_simt_stack[i]->resume(fname);
        m_simt_stack[i]->get_pdom_stack_top_info(&pc, &rpc);
        for (unsigned t = 0; t < m_config->warp_size; t++) {
          if (m_thread != NULL) {
            m_thread[i * m_config->warp_size + t]->set_npc(pc);
            m_thread[i * m_config->warp_size + t]->update_pc();
          }
        }
        start_pc = pc;
      }

      m_warp[i]->init(start_pc, cta_id, i, active_threads, m_dynamic_warp_id);
      ++m_dynamic_warp_id;
      m_not_completed += n_active;
      ++m_active_warps;
    }
  }
}

// return the next pc of a thread
address_type shader_core_ctx::next_pc(int tid) const {
  if (tid == -1) return -1;
  ptx_thread_info *the_thread = m_thread[tid];
  if (the_thread == NULL) return -1;
  return the_thread
      ->get_pc();  // PC should already be updatd to next PC at this point (was
                   // set in shader_decode() last time thread ran)
}

void gpgpu_sim::get_pdom_stack_top_info(unsigned sid, unsigned tid,
                                        unsigned *pc, unsigned *rpc) {
  unsigned cluster_id = m_shader_config->sid_to_cluster(sid);
  m_cluster[cluster_id]->get_pdom_stack_top_info(sid, tid, pc, rpc);
}

void shader_core_ctx::get_pdom_stack_top_info(unsigned tid, unsigned *pc,
                                              unsigned *rpc) const {
  unsigned warp_id = tid / m_config->warp_size;
  m_simt_stack[warp_id]->get_pdom_stack_top_info(pc, rpc);
}

float shader_core_ctx::get_current_occupancy(unsigned long long &active,
                                             unsigned long long &total) const {
  // To match the achieved_occupancy in nvprof, only SMs that are active are
  // counted toward the occupancy.
  if (m_active_warps > 0) {
    total += m_warp.size();
    active += m_active_warps;
    return float(active) / float(total);
  } else {
    return 0;
  }
}

void shader_core_stats::print(FILE *fout) const {
  unsigned long long thread_icount_uarch = 0;
  unsigned long long warp_icount_uarch = 0;

  for (unsigned i = 0; i < m_config->num_shader(); i++) {
    thread_icount_uarch += m_num_sim_insn[i];
    warp_icount_uarch += m_num_sim_winsn[i];
  }
  fprintf(fout, "gpgpu_n_tot_thrd_icount = %lld\n", thread_icount_uarch);
  fprintf(fout, "gpgpu_n_tot_w_icount = %lld\n", warp_icount_uarch);

  fprintf(fout, "gpgpu_n_stall_shd_mem = %d\n", gpgpu_n_stall_shd_mem);
  fprintf(fout, "gpgpu_n_mem_read_local = %d\n", gpgpu_n_mem_read_local);
  fprintf(fout, "gpgpu_n_mem_write_local = %d\n", gpgpu_n_mem_write_local);
  fprintf(fout, "gpgpu_n_mem_read_global = %d\n", gpgpu_n_mem_read_global);
  fprintf(fout, "gpgpu_n_mem_write_global = %d\n", gpgpu_n_mem_write_global);
  fprintf(fout, "gpgpu_n_mem_texture = %d\n", gpgpu_n_mem_texture);
  fprintf(fout, "gpgpu_n_mem_const = %d\n", gpgpu_n_mem_const);

  fprintf(fout, "gpgpu_n_load_insn  = %d\n", gpgpu_n_load_insn);
  fprintf(fout, "gpgpu_n_store_insn = %d\n", gpgpu_n_store_insn);
  fprintf(fout, "gpgpu_n_shmem_insn = %d\n", gpgpu_n_shmem_insn);
  fprintf(fout, "gpgpu_n_sstarr_insn = %d\n", gpgpu_n_sstarr_insn);
  fprintf(fout, "gpgpu_n_tex_insn = %d\n", gpgpu_n_tex_insn);
  fprintf(fout, "gpgpu_n_const_mem_insn = %d\n", gpgpu_n_const_insn);
  fprintf(fout, "gpgpu_n_param_mem_insn = %d\n", gpgpu_n_param_insn);

  fprintf(fout, "gpgpu_n_shmem_bkconflict = %d\n", gpgpu_n_shmem_bkconflict);
  fprintf(fout, "gpgpu_n_cache_bkconflict = %d\n", gpgpu_n_cache_bkconflict);

  fprintf(fout, "gpgpu_n_intrawarp_mshr_merge = %d\n",
          gpgpu_n_intrawarp_mshr_merge);
  fprintf(fout, "gpgpu_n_cmem_portconflict = %d\n", gpgpu_n_cmem_portconflict);

  c_mem_resource_stall = c_mem_resource_stall + gpu_stall_shd_mem_breakdown[C_MEM][BK_CONF];
  s_mem_bk_conf = s_mem_bk_conf + gpu_stall_shd_mem_breakdown[S_MEM][BK_CONF];
  gl_mem_resource_stall = gl_mem_resource_stall + gpu_stall_shd_mem_breakdown[G_MEM_LD][BK_CONF] +
          gpu_stall_shd_mem_breakdown[G_MEM_ST][BK_CONF] +
          gpu_stall_shd_mem_breakdown[L_MEM_LD][BK_CONF] +
          gpu_stall_shd_mem_breakdown[L_MEM_ST][BK_CONF];
  gl_mem_coal_stall = gl_mem_coal_stall + gpu_stall_shd_mem_breakdown[G_MEM_LD][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[G_MEM_ST][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[L_MEM_LD][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[L_MEM_ST]
                                     [COAL_STALL];
  gl_mem_data_port_stall = gl_mem_data_port_stall+ gpu_stall_shd_mem_breakdown[G_MEM_LD][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[G_MEM_ST][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[L_MEM_LD][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[L_MEM_ST]
                                         [DATA_PORT_STALL];

  fprintf(fout, "gpgpu_stall_shd_mem[c_mem][resource_stall] = %d\n",
          gpu_stall_shd_mem_breakdown[C_MEM][BK_CONF]);
  // fprintf(fout, "gpgpu_stall_shd_mem[c_mem][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[C_MEM][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[c_mem][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[C_MEM][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[c_mem][data_port_stall] = %d\n",
  // gpu_stall_shd_mem_breakdown[C_MEM][DATA_PORT_STALL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[t_mem][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[T_MEM][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[t_mem][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[T_MEM][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[t_mem][data_port_stall] = %d\n",
  // gpu_stall_shd_mem_breakdown[T_MEM][DATA_PORT_STALL]);
  fprintf(fout, "gpgpu_stall_shd_mem[s_mem][bk_conf] = %d\n",
          gpu_stall_shd_mem_breakdown[S_MEM][BK_CONF]);
  fprintf(
      fout, "gpgpu_stall_shd_mem[gl_mem][resource_stall] = %d\n",
      gpu_stall_shd_mem_breakdown[G_MEM_LD][BK_CONF] +
          gpu_stall_shd_mem_breakdown[G_MEM_ST][BK_CONF] +
          gpu_stall_shd_mem_breakdown[L_MEM_LD][BK_CONF] +
          gpu_stall_shd_mem_breakdown[L_MEM_ST][BK_CONF]);  // coalescing stall
                                                            // at data cache
  fprintf(
      fout, "gpgpu_stall_shd_mem[gl_mem][coal_stall] = %d\n",
      gpu_stall_shd_mem_breakdown[G_MEM_LD][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[G_MEM_ST][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[L_MEM_LD][COAL_STALL] +
          gpu_stall_shd_mem_breakdown[L_MEM_ST]
                                     [COAL_STALL]);  // coalescing stall + bank
                                                     // conflict at data cache
  fprintf(fout, "gpgpu_stall_shd_mem[gl_mem][data_port_stall] = %d\n",
          gpu_stall_shd_mem_breakdown[G_MEM_LD][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[G_MEM_ST][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[L_MEM_LD][DATA_PORT_STALL] +
              gpu_stall_shd_mem_breakdown[L_MEM_ST]
                                         [DATA_PORT_STALL]);  // data port stall
                                                              // at data cache
  // fprintf(fout, "gpgpu_stall_shd_mem[g_mem_ld][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_LD][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_ld][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_LD][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_ld][wb_icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_LD][WB_ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_ld][wb_rsrv_fail] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_LD][WB_CACHE_RSRV_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_st][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_ST][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_st][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_ST][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_st][wb_icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_ST][WB_ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[g_mem_st][wb_rsrv_fail] = %d\n",
  // gpu_stall_shd_mem_breakdown[G_MEM_ST][WB_CACHE_RSRV_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_LD][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_LD][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][wb_icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_LD][WB_ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][wb_rsrv_fail] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_LD][WB_CACHE_RSRV_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_st][mshr_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_ST][MSHR_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_st][icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_ST][ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][wb_icnt_rc] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_ST][WB_ICNT_RC_FAIL]); fprintf(fout,
  // "gpgpu_stall_shd_mem[l_mem_ld][wb_rsrv_fail] = %d\n",
  // gpu_stall_shd_mem_breakdown[L_MEM_ST][WB_CACHE_RSRV_FAIL]);

  fprintf(fout, "gpu_reg_bank_conflict_stalls = %d\n",
          gpu_reg_bank_conflict_stalls);

  fprintf(fout, "Warp Occupancy Distribution:\n");
  fprintf(fout, "Stall:%d\t", shader_cycle_distro[2]);
  fprintf(fout, "W0_Idle:%d\t", shader_cycle_distro[0]);
  fprintf(fout, "W0_Scoreboard:%d", shader_cycle_distro[1]);
  for (unsigned i = 3; i < m_config->warp_size + 3; i++)
    fprintf(fout, "\tW%d:%d", i - 2, shader_cycle_distro[i]);
  fprintf(fout, "\n");
  fprintf(fout, "single_issue_nums: ");
  for (unsigned i = 0; i < m_config->gpgpu_num_sched_per_core; i++)
    fprintf(fout, "WS%d:%d\t", i, single_issue_nums[i]);
  fprintf(fout, "\n");
  fprintf(fout, "dual_issue_nums: ");
  for (unsigned i = 0; i < m_config->gpgpu_num_sched_per_core; i++)
    fprintf(fout, "WS%d:%d\t", i, dual_issue_nums[i]);
  fprintf(fout, "\n");

  m_outgoing_traffic_stats->print(fout);
  m_incoming_traffic_stats->print(fout);
}

void shader_core_stats::event_warp_issued(unsigned s_id, unsigned warp_id,
                                          unsigned num_issued,
                                          unsigned dynamic_warp_id) {
  assert(warp_id <= m_config->max_warps_per_shader);
  for (unsigned i = 0; i < num_issued; ++i) {
    if (m_shader_dynamic_warp_issue_distro[s_id].size() <= dynamic_warp_id) {
      m_shader_dynamic_warp_issue_distro[s_id].resize(dynamic_warp_id + 1);
    }
    ++m_shader_dynamic_warp_issue_distro[s_id][dynamic_warp_id];
    if (m_shader_warp_slot_issue_distro[s_id].size() <= warp_id) {
      m_shader_warp_slot_issue_distro[s_id].resize(warp_id + 1);
    }
    ++m_shader_warp_slot_issue_distro[s_id][warp_id];
  }
}

void shader_core_stats::visualizer_print(gzFile visualizer_file) {
  // warp divergence breakdown
  gzprintf(visualizer_file, "WarpDivergenceBreakdown:");
  unsigned int total = 0;
  unsigned int cf =
      (m_config->gpgpu_warpdistro_shader == -1) ? m_config->num_shader() : 1;
  gzprintf(visualizer_file, " %d",
           (shader_cycle_distro[0] - last_shader_cycle_distro[0]) / cf);
  gzprintf(visualizer_file, " %d",
           (shader_cycle_distro[1] - last_shader_cycle_distro[1]) / cf);
  gzprintf(visualizer_file, " %d",
           (shader_cycle_distro[2] - last_shader_cycle_distro[2]) / cf);
  for (unsigned i = 0; i < m_config->warp_size + 3; i++) {
    if (i >= 3) {
      total += (shader_cycle_distro[i] - last_shader_cycle_distro[i]);
      if (((i - 3) % (m_config->warp_size / 8)) ==
          ((m_config->warp_size / 8) - 1)) {
        gzprintf(visualizer_file, " %d", total / cf);
        total = 0;
      }
    }
    last_shader_cycle_distro[i] = shader_cycle_distro[i];
  }
  gzprintf(visualizer_file, "\n");

  gzprintf(visualizer_file, "ctas_completed: %d\n", ctas_completed);
  ctas_completed = 0;
  // warp issue breakdown
  unsigned sid = m_config->gpgpu_warp_issue_shader;
  unsigned count = 0;
  unsigned warp_id_issued_sum = 0;
  gzprintf(visualizer_file, "WarpIssueSlotBreakdown:");
  if (m_shader_warp_slot_issue_distro[sid].size() > 0) {
    for (std::vector<unsigned>::const_iterator iter =
             m_shader_warp_slot_issue_distro[sid].begin();
         iter != m_shader_warp_slot_issue_distro[sid].end(); iter++, count++) {
      unsigned diff = count < m_last_shader_warp_slot_issue_distro.size()
                          ? *iter - m_last_shader_warp_slot_issue_distro[count]
                          : *iter;
      gzprintf(visualizer_file, " %d", diff);
      warp_id_issued_sum += diff;
    }
    m_last_shader_warp_slot_issue_distro = m_shader_warp_slot_issue_distro[sid];
  } else {
    gzprintf(visualizer_file, " 0");
  }
  gzprintf(visualizer_file, "\n");

#define DYNAMIC_WARP_PRINT_RESOLUTION 32
  unsigned total_issued_this_resolution = 0;
  unsigned dynamic_id_issued_sum = 0;
  count = 0;
  gzprintf(visualizer_file, "WarpIssueDynamicIdBreakdown:");
  if (m_shader_dynamic_warp_issue_distro[sid].size() > 0) {
    for (std::vector<unsigned>::const_iterator iter =
             m_shader_dynamic_warp_issue_distro[sid].begin();
         iter != m_shader_dynamic_warp_issue_distro[sid].end();
         iter++, count++) {
      unsigned diff =
          count < m_last_shader_dynamic_warp_issue_distro.size()
              ? *iter - m_last_shader_dynamic_warp_issue_distro[count]
              : *iter;
      total_issued_this_resolution += diff;
      if ((count + 1) % DYNAMIC_WARP_PRINT_RESOLUTION == 0) {
        gzprintf(visualizer_file, " %d", total_issued_this_resolution);
        dynamic_id_issued_sum += total_issued_this_resolution;
        total_issued_this_resolution = 0;
      }
    }
    if (count % DYNAMIC_WARP_PRINT_RESOLUTION != 0) {
      gzprintf(visualizer_file, " %d", total_issued_this_resolution);
      dynamic_id_issued_sum += total_issued_this_resolution;
    }
    m_last_shader_dynamic_warp_issue_distro =
        m_shader_dynamic_warp_issue_distro[sid];
    assert(warp_id_issued_sum == dynamic_id_issued_sum);
  } else {
    gzprintf(visualizer_file, " 0");
  }
  gzprintf(visualizer_file, "\n");

  // overall cache miss rates
  gzprintf(visualizer_file, "gpgpu_n_cache_bkconflict: %d\n",
           gpgpu_n_cache_bkconflict);
  gzprintf(visualizer_file, "gpgpu_n_shmem_bkconflict: %d\n",
           gpgpu_n_shmem_bkconflict);

  // instruction count per shader core
  gzprintf(visualizer_file, "shaderinsncount:  ");
  for (unsigned i = 0; i < m_config->num_shader(); i++)
    gzprintf(visualizer_file, "%u ", m_num_sim_insn[i]);
  gzprintf(visualizer_file, "\n");
  // warp instruction count per shader core
  gzprintf(visualizer_file, "shaderwarpinsncount:  ");
  for (unsigned i = 0; i < m_config->num_shader(); i++)
    gzprintf(visualizer_file, "%u ", m_num_sim_winsn[i]);
  gzprintf(visualizer_file, "\n");
  // warp divergence per shader core
  gzprintf(visualizer_file, "shaderwarpdiv: ");
  for (unsigned i = 0; i < m_config->num_shader(); i++)
    gzprintf(visualizer_file, "%u ", m_n_diverge[i]);
  gzprintf(visualizer_file, "\n");
}

#define PROGRAM_MEM_START                                      \
  0xF0000000 /* should be distinct from other memory spaces... \
                check ptx_ir.h to verify this does not overlap \
                other memory spaces */

const warp_inst_t *exec_shader_core_ctx::get_next_inst(unsigned warp_id,
                                                       address_type pc) {
  // read the inst from the functional model
  return m_gpu->gpgpu_ctx->ptx_fetch_inst(pc);
}

void exec_shader_core_ctx::get_pdom_stack_top_info(unsigned warp_id,
                                                   const warp_inst_t *pI,
                                                   unsigned *pc,
                                                   unsigned *rpc) {
  m_simt_stack[warp_id]->get_pdom_stack_top_info(pc, rpc);
}

const active_mask_t &exec_shader_core_ctx::get_active_mask(
    unsigned warp_id, const warp_inst_t *pI) {
  return m_simt_stack[warp_id]->get_active_mask();
}

void shader_core_ctx::decode() {
  if (m_inst_fetch_buffer.m_valid) {
    // decode 1 or 2 instructions and place them into ibuffer
    address_type pc = m_inst_fetch_buffer.m_pc;
    const warp_inst_t *pI1 = get_next_inst(m_inst_fetch_buffer.m_warp_id, pc);
    m_warp[m_inst_fetch_buffer.m_warp_id]->ibuffer_fill(0, pI1);
    m_warp[m_inst_fetch_buffer.m_warp_id]->inc_inst_in_pipeline();
    if (pI1) {
      m_stats->m_num_decoded_insn[m_sid]++;
      if (pI1->oprnd_type == INT_OP) {
        m_stats->m_num_INTdecoded_insn[m_sid]++;
      } else if (pI1->oprnd_type == FP_OP) {
        m_stats->m_num_FPdecoded_insn[m_sid]++;
      }
      const warp_inst_t *pI2 =
          get_next_inst(m_inst_fetch_buffer.m_warp_id, pc + pI1->isize);
      if (pI2) {
        m_warp[m_inst_fetch_buffer.m_warp_id]->ibuffer_fill(1, pI2);
        m_warp[m_inst_fetch_buffer.m_warp_id]->inc_inst_in_pipeline();
        m_stats->m_num_decoded_insn[m_sid]++;
        if (pI2->oprnd_type == INT_OP) {
          m_stats->m_num_INTdecoded_insn[m_sid]++;
        } else if (pI2->oprnd_type == FP_OP) {
          m_stats->m_num_FPdecoded_insn[m_sid]++;
        }
      }
    }
    m_inst_fetch_buffer.m_valid = false;
  }
}

void shader_core_ctx::fetch() {
  if (!m_inst_fetch_buffer.m_valid) {
    if (m_L1I->access_ready()) {
      mem_fetch *mf = m_L1I->next_access();
      m_warp[mf->get_wid()]->clear_imiss_pending();
      m_inst_fetch_buffer =
          ifetch_buffer_t(m_warp[mf->get_wid()]->get_pc(),
                          mf->get_access_size(), mf->get_wid());
      assert(m_warp[mf->get_wid()]->get_pc() ==
             (mf->get_addr() -
              PROGRAM_MEM_START));  // Verify that we got the instruction we
                                    // were expecting.
      m_inst_fetch_buffer.m_valid = true;
      m_warp[mf->get_wid()]->set_last_fetch(m_gpu->gpu_sim_cycle);
      delete mf;
    } else {
      // find an active warp with space in instruction buffer that is not
      // already waiting on a cache miss and get next 1-2 instructions from
      // i-cache...
      for (unsigned i = 0; i < m_config->max_warps_per_shader; i++) {
        unsigned warp_id =
            (m_last_warp_fetched + 1 + i) % m_config->max_warps_per_shader;

        // this code checks if this warp has finished executing and can be
        // reclaimed
        if (m_warp[warp_id]->hardware_done() &&
            !m_scoreboard->pendingWrites(warp_id, m_config->pending_write_ignore) &&
            !m_warp[warp_id]->done_exit() && !(m_config->gpgpu_reply_buffer && !m_warp[m_inst_fetch_buffer.m_warp_id]->replay_buffer_empty())) {
          bool did_exit = false;
          for (unsigned t = 0; t < m_config->warp_size; t++) {
            unsigned tid = warp_id * m_config->warp_size + t;
            if (m_threadState[tid].m_active == true) {
              m_threadState[tid].m_active = false;
              unsigned cta_id = m_warp[warp_id]->get_cta_id();
              if (m_thread[tid] == NULL) {
                register_cta_thread_exit(cta_id, m_kernel);
              } else {
                register_cta_thread_exit(cta_id,
                                         &(m_thread[tid]->get_kernel()));
              }
              m_not_completed -= 1;
              m_active_threads.reset(tid);
              did_exit = true;
            }
          }
          if (did_exit) m_warp[warp_id]->set_done_exit();
          --m_active_warps;
          assert(m_active_warps >= 0);
        }

        // this code fetches instructions from the i-cache or generates memory
        if (!m_warp[warp_id]->functional_done() &&
            !m_warp[warp_id]->imiss_pending() &&
            m_warp[warp_id]->ibuffer_empty()) {
          address_type pc;
          pc = m_warp[warp_id]->get_pc();
          address_type ppc = pc + PROGRAM_MEM_START;
          unsigned nbytes = 16;
          unsigned offset_in_block =
              pc & (m_config->m_L1I_config.get_line_sz() - 1);
          if ((offset_in_block + nbytes) > m_config->m_L1I_config.get_line_sz())
            nbytes = (m_config->m_L1I_config.get_line_sz() - offset_in_block);

          // TODO: replace with use of allocator
          // mem_fetch *mf = m_mem_fetch_allocator->alloc()
          mem_access_t acc(INST_ACC_R, ppc, nbytes, false, m_gpu->gpgpu_ctx);
          mem_fetch *mf = new mem_fetch(
              acc, NULL /*we don't have an instruction yet*/, READ_PACKET_SIZE,
              warp_id, m_sid, m_tpc, m_memory_config,
              m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle);
          std::list<cache_event> events;
          enum cache_request_status status;
          if (m_config->perfect_inst_const_cache)
            status = HIT;
          else
            status = m_L1I->access(
                (new_addr_type)ppc, mf,
                m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle, events);

          if (status == MISS) {
            m_last_warp_fetched = warp_id;
            m_warp[warp_id]->set_imiss_pending();
            m_warp[warp_id]->set_last_fetch(m_gpu->gpu_sim_cycle);
          } else if (status == HIT) {
            m_last_warp_fetched = warp_id;
            m_inst_fetch_buffer = ifetch_buffer_t(pc, nbytes, warp_id);
            m_warp[warp_id]->set_last_fetch(m_gpu->gpu_sim_cycle);
            delete mf;
          } else {
            m_last_warp_fetched = warp_id;
            assert(status == RESERVATION_FAIL);
            delete mf;
          }
          break;
        }
      }
    }
  }

  m_L1I->cycle();
}

void exec_shader_core_ctx::func_exec_inst(warp_inst_t &inst) {
  execute_warp_inst_t(inst);
  if (inst.is_load() || inst.is_store()) {
    inst.generate_mem_accesses();
    // inst.print_m_accessq();
  }
}

void exec_shader_core_ctx::func_exec_inst_updatePCOnly(warp_inst_t &inst, int warp_num) {
  //execute_warp_inst_t_updatePCOnly(inst, warp_num);
  execute_warp_inst_t(inst, warp_num);
}

void exec_shader_core_ctx::func_exec_inst_ExecInstOnly(warp_inst_t &inst, int warp_num) {
  //execute_warp_inst_t_ExecInstOnly(inst, warp_num);
  if (inst.is_load() || inst.is_store()) {
    inst.generate_mem_accesses();
    if (m_config->gpgpu_perfect_mem_data)
    { 
      if((inst.space.get_type() == global_space || inst.space.get_type() == local_space || inst.space.get_type() == shared_space) && (inst.is_load() || inst.is_store()))
        inst.space.set_type(const_space); 
    }
    m_scoreboard->appendMemStatus(inst,inst.space.get_type());
  }
}

bool exec_shader_core_ctx::isSyncInstCore(const warp_inst_t *inst, int warp_num)
{
  return isSyncInstExec(inst, warp_num);
}

bool shader_core_ctx::isSyncInst(const warp_inst_t *inst, int warp_num)
{
  return isSyncInstCore(inst, warp_num);
}

void shader_core_ctx::issue_warp(register_set &pipe_reg_set,
                                 const warp_inst_t *next_inst,
                                 const active_mask_t &active_mask,
                                 unsigned warp_id, unsigned sch_id, int sid) {
  warp_inst_t **pipe_reg =
      pipe_reg_set.get_free(m_config->sub_core_model, sch_id);
  int reg_id = pipe_reg_set.get_free_id(m_config->sub_core_model, sch_id);

  assert(pipe_reg);

  m_warp[warp_id]->ibuffer_free();
  assert(next_inst->valid());
  **pipe_reg = *next_inst;  // static instruction information
  (*pipe_reg)->set_sid(sid);
  (*pipe_reg)->set_cycle_issued(cycles_passed);
  (*pipe_reg)->set_cycle_issued_warp(cycles_passed);
  (*pipe_reg)->issue(active_mask, warp_id,
                     m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle,
                     m_warp[warp_id]->get_dynamic_warp_id(),
                     sch_id);  // dynamic instruction information
  m_stats->shader_cycle_distro[2 + (*pipe_reg)->active_count()]++;
  func_exec_inst(**pipe_reg);
  if (next_inst->op == BARRIER_OP && !m_config->ignore_synchronization) {
    m_warp[warp_id]->store_info_of_last_inst_at_barrier(*pipe_reg);
    m_barriers.warp_reaches_barrier(m_warp[warp_id]->get_cta_id(), warp_id,
                                    const_cast<warp_inst_t *>(next_inst));
  } else if (next_inst->op == MEMORY_BARRIER_OP) {
    m_warp[warp_id]->set_membar();
  }
  int checker = 0;
  updateSIMTStack(warp_id, *pipe_reg);

  int status = 0;

  if((next_inst->op==LOAD_OP ||
    next_inst->op==TENSOR_CORE_LOAD_OP ||
    next_inst->op==TENSOR_CORE_STORE_OP ||
    next_inst->op==STORE_OP ||
    next_inst->op == MEMORY_BARRIER_OP))
    {
	    m_scoreboard->reserveRegistersMem(*pipe_reg);
      status = 1;
      inst_counter ++;
    }
  else
    {
	    m_scoreboard->reserveRegistersComp(*pipe_reg);
      status = 4;
    }

  m_scoreboard->reserveRegisters(*pipe_reg,m_config->gpgpu_perfect_mem_data,status);

  m_warp[warp_id]->set_next_pc(next_inst->pc + next_inst->isize);
}

// Put instruction in replay queue
void shader_core_ctx::issue_warp_push_in_replay(register_set &pipe_reg_set,
                                 const warp_inst_t *next_inst,
                                 const active_mask_t &active_mask,
                                 unsigned warp_id, unsigned sch_id, int replay_buffer_idx, int pc_num, int sid, int MEM_ON) {

  m_warp[warp_id]->ibuffer_free();
  warp_inst_t *pipe_reg = const_cast<warp_inst_t*>(next_inst);
  (pipe_reg)->set_cycle_issued(cycles_passed);
  (pipe_reg)->issue_push_to_replay(active_mask, warp_id,
                     m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle,
                     m_warp[warp_id]->get_dynamic_warp_id(),
                     sch_id);  // dynamic instruction information
  func_exec_inst_updatePCOnly(*pipe_reg, warp_id);
  updateSIMTStack(warp_id, pipe_reg);
  // fill inst in OOO replay buffer

  if(!MEM_ON)
    m_warp[warp_id]->replay_buffer_fill(replay_buffer_idx,pipe_reg,pc_num);
  else
    m_warp[warp_id]->replay_buffer_fill_mem(replay_buffer_idx,pipe_reg,pc_num);

  m_warp[warp_id]->set_next_pc(next_inst->pc + next_inst->isize);
  m_warp[warp_id]->ibuffer_step();
}

void shader_core_ctx::issue_warp_push_in_replay_mem(register_set &pipe_reg_set,
                                 const warp_inst_t *next_inst,
                                 const active_mask_t &active_mask,
                                 unsigned warp_id, unsigned sch_id, int replay_buffer_idx, int pc_num, int sid, int MEM_ON) {

  m_warp[warp_id]->ibuffer_free();
  warp_inst_t *pipe_reg = const_cast<warp_inst_t*>(next_inst);
  (pipe_reg)->set_cycle_issued(cycles_passed);
  (pipe_reg)->set_cycle_issued_warp(cycles_passed);
  (pipe_reg)->issue_push_to_replay(active_mask, warp_id,
                     m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle,
                     m_warp[warp_id]->get_dynamic_warp_id(),
                     sch_id);  // dynamic instruction information
  func_exec_inst_updatePCOnly(*pipe_reg, warp_id);
  updateSIMTStack(warp_id, pipe_reg);
  // fill inst in OOO replay buffer
  m_warp[warp_id]->replay_buffer_fill_mem(replay_buffer_idx,pipe_reg,pc_num);

  m_warp[warp_id]->set_next_pc(next_inst->pc + next_inst->isize);
  m_warp[warp_id]->ibuffer_step();
}

// push inst from replay
void shader_core_ctx::issue_warp_push_from_replay(register_set &pipe_reg_set,
                                 const warp_inst_t *next_inst,
                                 const active_mask_t &active_mask,
                                 unsigned warp_id, unsigned sch_id, int sid, int MEM_ON) {
  warp_inst_t **pipe_reg =
      pipe_reg_set.get_free(m_config->sub_core_model, sch_id);
  int reg_id = pipe_reg_set.get_free_id(m_config->sub_core_model, sch_id);

  assert(pipe_reg);

  if(!MEM_ON)
    m_warp[warp_id]->replay_buffer_free();
  else  
    m_warp[warp_id]->replay_buffer_free_mem();

  assert(next_inst->valid());
  **pipe_reg = *next_inst;  // static instruction information
  (*pipe_reg)->set_sid(sid);
  (*pipe_reg)->issue_push_from_replay(active_mask, warp_id,
                     m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle,
                     m_warp[warp_id]->get_dynamic_warp_id(),
                     sch_id);  // dynamic instruction information
  m_stats->shader_cycle_distro[2 + (*pipe_reg)->active_count()]++;
  func_exec_inst_ExecInstOnly(**pipe_reg, warp_id);
  if (next_inst->op == BARRIER_OP && !m_config->ignore_synchronization) {
    m_warp[warp_id]->store_info_of_last_inst_at_barrier(*pipe_reg);
    m_barriers.warp_reaches_barrier(m_warp[warp_id]->get_cta_id(), warp_id,
                                    const_cast<warp_inst_t *>(next_inst));
  } else if (next_inst->op == MEMORY_BARRIER_OP) {
    m_warp[warp_id]->set_membar();
  }

  int status = 0;

  if((next_inst->op==LOAD_OP ||
    next_inst->op==TENSOR_CORE_LOAD_OP ||
    next_inst->op==TENSOR_CORE_STORE_OP ||
    next_inst->op==STORE_OP ||
    next_inst->op == MEMORY_BARRIER_OP))
    {
	    m_scoreboard->reserveRegistersMem(*pipe_reg);
      status = 1;
      inst_counter ++;
    }
  else
    {
	    m_scoreboard->reserveRegistersComp(*pipe_reg);
      status = 4;
    }

  m_scoreboard->reserveRegisters(*pipe_reg,m_config->gpgpu_perfect_mem_data,status);
}

void shader_core_ctx::issue_warp_push_from_replay_mem(register_set &pipe_reg_set,
                                 const warp_inst_t *next_inst,
                                 const active_mask_t &active_mask,
                                 unsigned warp_id, unsigned sch_id, int sid, int MEM_ON) {
  warp_inst_t **pipe_reg =
      pipe_reg_set.get_free(m_config->sub_core_model, sch_id);
  int reg_id = pipe_reg_set.get_free_id(m_config->sub_core_model, sch_id);

  assert(pipe_reg);
  m_warp[warp_id]->replay_buffer_free_mem();
  assert(next_inst->valid());
  **pipe_reg = *next_inst;  // static instruction information
  (*pipe_reg)->set_sid(sid);
  (*pipe_reg)->issue_push_from_replay(active_mask, warp_id,
                     m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle,
                     m_warp[warp_id]->get_dynamic_warp_id(),
                     sch_id);  // dynamic instruction information
  m_stats->shader_cycle_distro[2 + (*pipe_reg)->active_count()]++;
  func_exec_inst_ExecInstOnly(**pipe_reg, warp_id);
  if (next_inst->op == BARRIER_OP && !m_config->ignore_synchronization) {
    m_warp[warp_id]->store_info_of_last_inst_at_barrier(*pipe_reg);
    m_barriers.warp_reaches_barrier(m_warp[warp_id]->get_cta_id(), warp_id,
                                    const_cast<warp_inst_t *>(next_inst));
  } else if (next_inst->op == MEMORY_BARRIER_OP) {
    m_warp[warp_id]->set_membar();
  }

  int status = 0;

  if((next_inst->op==LOAD_OP ||
    next_inst->op==TENSOR_CORE_LOAD_OP ||
    next_inst->op==TENSOR_CORE_STORE_OP ||
    next_inst->op==STORE_OP ||
    next_inst->op == MEMORY_BARRIER_OP))
    {
	    m_scoreboard->reserveRegistersMem(*pipe_reg);
      status = 1;
      inst_counter ++;
    }
  else
    {
	    m_scoreboard->reserveRegistersComp(*pipe_reg);
      status = 4;
    }

  m_scoreboard->reserveRegisters(*pipe_reg,m_config->gpgpu_perfect_mem_data,status);
}

void shader_core_ctx::issue() {
  // Ensure fair round robin issu between schedulers
  unsigned j;
  num_of_schedulers =  schedulers.size();
  for (unsigned i = 0; i < schedulers.size(); i++) {
    j = (Issue_Prio + i) % schedulers.size();
    schedulers[j]->cycle(m_cluster->m_cluster_id);
  }
  Issue_Prio = (Issue_Prio + 1) % schedulers.size();

  // really is issue;
  // for (unsigned i = 0; i < schedulers.size(); i++) {
  //    schedulers[i]->cycle();
  //}
}

shd_warp_t &scheduler_unit::warp(int i) { return *((*m_warp)[i]); }

/**
 * A general function to order things in a Loose Round Robin way. The simplist
 * use of this function would be to implement a loose RR scheduler between all
 * the warps assigned to this core. A more sophisticated usage would be to order
 * a set of "fetch groups" in a RR fashion. In the first case, the templated
 * class variable would be a simple unsigned int representing the warp_id.  In
 * the 2lvl case, T could be a struct or a list representing a set of warp_ids.
 * @param result_list: The resultant list the caller wants returned.  This list
 * is cleared and then populated in a loose round robin way
 * @param input_list: The list of things that should be put into the
 * result_list. For a simple scheduler this can simply be the m_supervised_warps
 * list.
 * @param last_issued_from_input:  An iterator pointing the last member in the
 * input_list that issued. Since this function orders in a RR fashion, the
 * object pointed to by this iterator will be last in the prioritization list
 * @param num_warps_to_add: The number of warps you want the scheudler to pick
 * between this cycle. Normally, this will be all the warps availible on the
 * core, i.e. m_supervised_warps.size(). However, a more sophisticated scheduler
 * may wish to limit this number. If the number if < m_supervised_warps.size(),
 * then only the warps with highest RR priority will be placed in the
 * result_list.
 */
template <class T>
void scheduler_unit::order_lrr(
    std::vector<T> &result_list, const typename std::vector<T> &input_list,
    const typename std::vector<T>::const_iterator &last_issued_from_input,
    unsigned num_warps_to_add) {
    
  // Addition
  // COLLECT STATS START

  m_mem_warp_next_cycle.clear();
  m_comp_warp_next_cycle.clear();

  for (std::vector<shd_warp_t *>::const_iterator iter =
  input_list.begin(); iter != input_list.end(); iter++) 
  {
    if ((*iter) != NULL && !(*iter)->done_exit())
    {
      int warp_id = (*iter)->get_warp_id();  
      if(!warp(warp_id).ibuffer_empty())
      {
        const warp_inst_t *pI = warp(warp_id).ibuffer_next_inst();
        if((pI->op==LOAD_OP ||
        pI->op==TENSOR_CORE_LOAD_OP ||
        pI->op==TENSOR_CORE_STORE_OP ||
        pI->op==STORE_OP ||
        pI->op == MEMORY_BARRIER_OP))
        {
          m_mem_warp_next_cycle.push_back(*iter);
        }
        else
        {
          m_comp_warp_next_cycle.push_back(*iter);
        }
      }
    }
  }

  // Original
  assert(num_warps_to_add <= input_list.size());
  result_list.clear();
  typename std::vector<T>::const_iterator iter =
      (last_issued_from_input == input_list.end()) ? input_list.begin()
                                                   : last_issued_from_input + 1;

  for (unsigned count = 0; count < num_warps_to_add; ++iter, ++count) {
    if (iter == input_list.end()) {
      iter = input_list.begin();
    }
    result_list.push_back(*iter);
  }
}

/*
 fast scheduler
 Takes feedback from Shader-> Memory interconnect to choose whether to schedule memory
 instructions or compute instructions -> it currently LRRs over all the warps inside 
 selection (mem/compute)
*/
template <class T>
void scheduler_unit::order_fast(
std::vector<T> &result_list, const typename std::vector<T> &input_list,
  const typename std::vector<T>::const_iterator &last_issued_from_input,
  unsigned num_warps_to_add, int m_cluster_id) {

    assert(num_warps_to_add <= input_list.size());
    result_list.clear();
    m_mem_warp_next_cycle.clear();
    m_comp_warp_next_cycle.clear();
    for (std::vector<shd_warp_t *>::const_iterator iter =
    input_list.begin(); iter != input_list.end(); iter++) 
    {
      if ((*iter) != NULL && !(*iter)->done_exit())
      {
        int warp_id = (*iter)->get_warp_id();  
        if(!warp(warp_id).ibuffer_empty())
        {
          const warp_inst_t *pI = warp(warp_id).ibuffer_next_inst();
          if((pI->op==LOAD_OP ||
          pI->op==TENSOR_CORE_LOAD_OP ||
          pI->op==TENSOR_CORE_STORE_OP ||
          pI->op==STORE_OP ||
          pI->op == MEMORY_BARRIER_OP))
          {
            m_mem_warp_next_cycle.push_back(*iter);
          }
          else
          {
            m_comp_warp_next_cycle.push_back(*iter);
          }
        }
      }
    }

    // ORIG

    //if((going_from_shader_to_mem <= 7) && !m_mem_warp_next_cycle.empty() )
    //if(!m_mem_warp_next_cycle.empty())
    if((icnt_pressure[m_cluster_id] < tot_icnt_buffer) && !m_mem_warp_next_cycle.empty())
    {
      typename std::vector<T>::const_iterator iter = m_mem_warp_next_cycle.begin();
      for (unsigned count = 0; count < m_mem_warp_next_cycle.size(); ++iter, ++count) {
        if (iter == m_mem_warp_next_cycle.end()) {
          iter = m_mem_warp_next_cycle.begin();
        }
        {
          result_list.push_back(*iter);
        }
      }

      iter = m_comp_warp_next_cycle.begin();
        for (unsigned count = 0; count < m_comp_warp_next_cycle.size(); ++iter, ++count) {
        if (iter == m_comp_warp_next_cycle.end()) {
          iter = m_comp_warp_next_cycle.begin();
        }
        {
          result_list.push_back(*iter);
        }
      }
    }
    else if(!m_comp_warp_next_cycle.empty())
    {
      // Send Compute instructions
      typename std::vector<T>::const_iterator iter = m_comp_warp_next_cycle.begin();
      for (unsigned count = 0; count < m_comp_warp_next_cycle.size(); ++iter, ++count) {
        if (iter == m_comp_warp_next_cycle.end()) {
          iter = m_comp_warp_next_cycle.begin();
        }
        {
          result_list.push_back(*iter);
        }
      }

      iter = m_mem_warp_next_cycle.begin();
      for (unsigned count = 0; count < m_mem_warp_next_cycle.size(); ++iter, ++count) {
        if (iter == m_mem_warp_next_cycle.end()) {
          iter = m_mem_warp_next_cycle.begin();
        }
        {
          result_list.push_back(*iter);
        }
      }
    }
    else
  {
      typename std::vector<T>::const_iterator iter =
      (last_issued_from_input == input_list.end()) ? input_list.begin()
                                                  : last_issued_from_input + 1;
      for (unsigned count = 0; count < num_warps_to_add; ++iter, ++count) {
      if (iter == input_list.end()) {
        iter = input_list.begin();
      }
      result_list.push_back(*iter);
    }
    
  }
}

// replay fast scheduler

template <class T>
void scheduler_unit::replay_order_fast(
std::vector<T> &result_list, const typename std::vector<T> &input_list,
  const typename std::vector<T>::const_iterator &last_issued_from_input,
  unsigned num_warps_to_add, int m_cluster_id, int MEM_ON) {

    assert(num_warps_to_add <= input_list.size());
    result_list.clear();
    m_mem_warp_next_cycle.clear();
    m_comp_warp_next_cycle.clear();

    if(MEM_ON)
    {
      assert(num_warps_to_add <= input_list.size());
      result_list.clear();
      typename std::vector<T>::const_iterator iter =
          (last_issued_from_input == input_list.end()) ? input_list.begin()
                                                      : last_issued_from_input + 1;

      for (unsigned count = 0; count < num_warps_to_add; ++iter, ++count) {
        if (iter == input_list.end()) {
          iter = input_list.begin();
        }
        result_list.push_back(*iter);
      }
    }
    else
    {
      for (std::vector<shd_warp_t *>::const_iterator iter =
      input_list.begin(); iter != input_list.end(); iter++) 
      {
        if ((*iter) != NULL && !(*iter)->done_exit())
        {
          int warp_id = (*iter)->get_warp_id();  
          if(!warp(warp_id).replay_buffer_empty())
          {
            const warp_inst_t *pI = warp(warp_id).replay_buffer_next_inst();
            if((pI->op==LOAD_OP ||
            pI->op==TENSOR_CORE_LOAD_OP ||
            pI->op==TENSOR_CORE_STORE_OP ||
            pI->op==STORE_OP ||
            pI->op == MEMORY_BARRIER_OP))
            {
              m_mem_warp_next_cycle.push_back(*iter);
            }
            else
            {
              m_comp_warp_next_cycle.push_back(*iter);
            }
          }
        }
      }

      // OLD
      //if((going_from_shader_to_mem <= 7) && !m_mem_warp_next_cycle.empty())
      //if(!m_mem_warp_next_cycle.empty())
      if((icnt_pressure[m_cluster_id] < tot_icnt_buffer) && !m_mem_warp_next_cycle.empty())
      {
        typename std::vector<T>::const_iterator iter = m_mem_warp_next_cycle.begin();
        for (unsigned count = 0; count < m_mem_warp_next_cycle.size(); ++iter, ++count) {
          if (iter == m_mem_warp_next_cycle.end()) {
            iter = m_mem_warp_next_cycle.begin();
          }
          {
            result_list.push_back(*iter);
          }
        }

        iter = m_comp_warp_next_cycle.begin();
          for (unsigned count = 0; count < m_comp_warp_next_cycle.size(); ++iter, ++count) {
          if (iter == m_comp_warp_next_cycle.end()) {
            iter = m_comp_warp_next_cycle.begin();
          }
          {
            result_list.push_back(*iter);
          }
        }
      }
      else if(!m_comp_warp_next_cycle.empty())
      {
        // Send Compute instructions
        typename std::vector<T>::const_iterator iter = m_comp_warp_next_cycle.begin();
        for (unsigned count = 0; count < m_comp_warp_next_cycle.size(); ++iter, ++count) {
          if (iter == m_comp_warp_next_cycle.end()) {
            iter = m_comp_warp_next_cycle.begin();
          }
          {
            result_list.push_back(*iter);
          }
        }

        iter = m_mem_warp_next_cycle.begin();
        for (unsigned count = 0; count < m_mem_warp_next_cycle.size(); ++iter, ++count) {
          if (iter == m_mem_warp_next_cycle.end()) {
            iter = m_mem_warp_next_cycle.begin();
          }
          {
            result_list.push_back(*iter);
          }
        }
      }
      else
      {
        typename std::vector<T>::const_iterator iter =
        (last_issued_from_input == input_list.end()) ? input_list.begin()
                                                    : last_issued_from_input + 1;
        for (unsigned count = 0; count < num_warps_to_add; ++iter, ++count) {
          if (iter == input_list.end()) {
            iter = input_list.begin();
          }
          result_list.push_back(*iter);
        }
      }
    }
}

/**
 * A general function to order things in an priority-based way.
 * The core usage of the function is similar to order_lrr.
 * The explanation of the additional parameters (beyond order_lrr) explains the
 * further extensions.
 * @param ordering: An enum that determines how the age function will be treated
 * in prioritization see the definition of OrderingType.
 * @param priority_function: This function is used to sort the input_list.  It
 * is passed to stl::sort as the sorting fucntion. So, if you wanted to sort a
 * list of integer warp_ids with the oldest warps having the most priority, then
 * the priority_function would compare the age of the two warps.
 */
template <class T>
void scheduler_unit::order_by_priority(
    std::vector<T> &result_list, const typename std::vector<T> &input_list,
    const typename std::vector<T>::const_iterator &last_issued_from_input,
    unsigned num_warps_to_add, OrderingType ordering,
    bool (*priority_func)(T lhs, T rhs)) {
  assert(num_warps_to_add <= input_list.size());
  result_list.clear();
  typename std::vector<T> temp = input_list;

  if (ORDERING_GREEDY_THEN_PRIORITY_FUNC == ordering) {
    T greedy_value = *last_issued_from_input;
    result_list.push_back(greedy_value);

    std::sort(temp.begin(), temp.end(), priority_func);
    typename std::vector<T>::iterator iter = temp.begin();
    for (unsigned count = 0; count < num_warps_to_add; ++count, ++iter) {
      if (*iter != greedy_value) {
        result_list.push_back(*iter);
      }
    }
  } else if (ORDERED_PRIORITY_FUNC_ONLY == ordering) {
    std::sort(temp.begin(), temp.end(), priority_func);
    typename std::vector<T>::iterator iter = temp.begin();
    for (unsigned count = 0; count < num_warps_to_add; ++count, ++iter) {
      result_list.push_back(*iter);
    }
  } else {
    fprintf(stderr, "Unknown ordering - %d\n", ordering);
    abort();
  }
}

void scheduler_unit::verify_stall(int warp_id, exec_unit_type_t type) {
  if (warp_id > actw) actw = warp_id;
  
  act_warp[m_shader->get_sid()][warp_id] = get_schd_id() + 1;
  
  // Don't consider warps that are not yet valid
  if (((*m_warp)[warp_id]) == NULL || warp(warp_id).done_exit()) {
    stallData[m_shader->get_sid()][warp_id][idlew]=1;
    return;
  }

  // Maybe warp is done and just waiting for pending writes
  if (warp(warp_id).hardware_done()) {
    if (m_scoreboard->pendingWrites(warp_id))
     stallData[m_shader->get_sid()][warp_id][pendingWritew]=1;
    //else
    // stallData[m_shader->get_sid()][warp_id][idlew]=2;
    return;
  }
  
  exec_unit_type_t previous_issued_inst_exec_type = type;
  bool diff_exec_units =
      m_shader->m_config
          ->gpgpu_dual_issue_diff_exec_units;

  if (warp(warp_id).waiting_barrier())
  {
    stallData[m_shader->get_sid()][warp_id][synco]=1;
  }

  if (warp(warp_id).waiting_fence())
  {
    stallData[m_shader->get_sid()][warp_id][pendingWritew]=1;
  }

  if (warp(warp_id).waiting())
  {
    stallData[m_shader->get_sid()][warp_id][idlew]=1;
    return;
  }

  if (warp(warp_id).ibuffer_empty() && !warp(warp_id).imiss_pending() && !warp(warp_id).functional_done())
  {
    if(m_shader->m_config->perfect_inst_const_cache)
    {
      m_shader->fetch();
      m_shader->decode();
    }
    if (warp(warp_id).ibuffer_empty())
     //stallData[m_shader->get_sid()][warp_id][ibufferw]=1;
     stallData[m_shader->get_sid()][warp_id][mem_data]=1;
  }

  if (warp(warp_id).imiss_pending() && !warp(warp_id).functional_done())
  {
    if(m_shader->m_config->perfect_inst_const_cache)
    {
      m_shader->fetch();
      m_shader->decode();
    }
    if (warp(warp_id).imiss_pending())
     //stallData[m_shader->get_sid()][warp_id][ibufferw]=1;
     stallData[m_shader->get_sid()][warp_id][mem_data]=1;
  }
  
  // We check control with whatever was in the buffer, otherwise
  // pc will not differ
  warp_inst_t *pISave = NULL;
  unsigned pc = warp(warp_id).get_pc();
  unsigned rpc;
  bool buffer_inst_good = false;
  bool valid = true;

  if (!warp(warp_id).ibuffer_empty() and !warp(warp_id).imiss_pending())
  {
    const warp_inst_t * pIControl = warp(warp_id).ibuffer_next_inst();
    pISave = (warp_inst_t *) pIControl;

    if (pIControl && pIControl->m_is_cdp && warp(warp_id).m_cdp_latency > 0) {
      stallData[m_shader->get_sid()][warp_id][others]=1;
      return;
    }

    valid = warp(warp_id).ibuffer_next_valid();

    if (pIControl) {
      m_shader->get_pdom_stack_top_info(warp_id, pIControl, &pc, &rpc);
      if (pc != pIControl->pc) {
        if(!warp(warp_id).ibuffer_empty() && !warp(warp_id).imiss_pending())
          stallData[m_shader->get_sid()][warp_id][control]=1;
        pc = pIControl->pc;
      }
      else
      {
        buffer_inst_good = true;
      }
    }
    //else if (valid) {
    // stallData[m_shader->get_sid()][warp_id][control]=1;
    //}
  }

  // For stall purposes we get the next instruction even if 
  // if the ibuffer is empty. But we do not modify the state of the warp
  const warp_inst_t *pI = buffer_inst_good ? (const warp_inst_t *) pISave : m_shader->get_next_inst(warp_id, pc);

  if (pI && pI->m_is_cdp && warp(warp_id).m_cdp_latency > 0) {
    stallData[m_shader->get_sid()][warp_id][others]=1;
    return;
  }

  if(!pI)
  {
    stallData[m_shader->get_sid()][warp_id][others]=1;
  }

  bool warp_inst_issued = false;
  if(!warp(warp_id).ibuffer_empty() && pI && !warp(warp_id).imiss_pending()){

    std::vector<int> ResComp = (m_scoreboard->checkCollisionComp(warp_id, pI));
    if(ResComp[0]){
      stallData[m_shader->get_sid()][warp_id][comp_data]=1;
    }

    std::vector<int> ResMem = (m_scoreboard->checkCollisionMem(warp_id, pI));
    if(ResMem[0]){
      stallData[m_shader->get_sid()][warp_id][mem_data]=1;
      //stallData[m_shader->get_sid()][warp_id][ibufferw]=1;
    }

    stallData[m_shader->get_sid()][warp_id][reserve_comp]=ResComp[1];
    stallData[m_shader->get_sid()][warp_id][release_comp]=ResComp[2];

    stallData[m_shader->get_sid()][warp_id][reserve_mem]=ResMem[1];
    stallData[m_shader->get_sid()][warp_id][release_mem]=ResMem[2];

    // Get inst is going to which structural unit, Mem or Compute
     if( pI->op == SP_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = sp_inst;
      else if( pI->op == SFU_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = sfu_inst;
      else if( pI->op == TENSOR_CORE_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = tensor_inst;
      else if( pI->op == DP_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = dp_inst;
      else if( pI->op == INTP_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = int_inst;
      else if( pI->op == ALU_SFU_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = alu_sfu;
      else if( pI->op >= SPEC_UNIT_START_ID)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = spec_inst;
      else if (pI->op == BARRIER_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = barrier_op;

    stallData[m_shader->get_sid()][warp_id][block_id] = warp(warp_id).get_cta_id();

    if ((pI->op == LOAD_OP) || (pI->op == STORE_OP) ||
        (pI->op == MEMORY_BARRIER_OP) ||
        (pI->op == TENSOR_CORE_LOAD_OP) ||
        (pI->op == TENSOR_CORE_STORE_OP)) {
      if (pI->op == MEMORY_BARRIER_OP)
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = barrier_mem;
      else
        stallData[m_shader->get_sid()][warp_id][OP_TYPE] = mem_inst;         
      if (m_mem_out->has_free(m_shader->m_config->sub_core_model, m_id)
        && (!diff_exec_units || previous_issued_inst_exec_type != exec_unit_type_t::MEM))
      {}
      else
      {
        stallData[m_shader->get_sid()][warp_id][mem_str]=1;
      }
    }
    else {
  
      bool sp_pipe_avail =
          (m_shader->m_config->gpgpu_num_sp_units > 0) &&
          m_sp_out->has_free(m_shader->m_config->sub_core_model, m_id);
      bool sfu_pipe_avail =
          (m_shader->m_config->gpgpu_num_sfu_units > 0) &&
          m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);
      bool tensor_core_pipe_avail =
          (m_shader->m_config->gpgpu_num_tensor_core_units > 0) &&
          m_tensor_core_out->has_free(
              m_shader->m_config->sub_core_model, m_id);
      bool dp_pipe_avail =
          (m_shader->m_config->gpgpu_num_dp_units > 0) &&
          m_dp_out->has_free(m_shader->m_config->sub_core_model, m_id);
      bool int_pipe_avail =
          (m_shader->m_config->gpgpu_num_int_units > 0) &&
          m_int_out->has_free(m_shader->m_config->sub_core_model, m_id);

      // This code need to be refactored
      if (pI->op != TENSOR_CORE_OP && pI->op != SFU_OP &&
          pI->op != DP_OP && !(pI->op >= SPEC_UNIT_START_ID)) {
        bool execute_on_SP = false;
        bool execute_on_INT = false;

        // if INT unit pipline exist, then execute ALU and INT
        // operations on INT unit and SP-FPU on SP unit (like in Volta)
        // if INT unit pipline does not exist, then execute all ALU, INT
        // and SP operations on SP unit (as in Fermi, Pascal GPUs)
        if (m_shader->m_config->gpgpu_num_int_units > 0 &&
            int_pipe_avail && pI->op != SP_OP &&
            !(diff_exec_units &&
              previous_issued_inst_exec_type == exec_unit_type_t::INT))
          {
            execute_on_INT = true;
          }
        else if (sp_pipe_avail &&
                  (m_shader->m_config->gpgpu_num_int_units == 0 ||
                  (m_shader->m_config->gpgpu_num_int_units > 0 &&
                    pI->op == SP_OP)) &&
                  !(diff_exec_units && previous_issued_inst_exec_type ==
                                          exec_unit_type_t::SP))
          {
            execute_on_SP = true;
          }
        if (execute_on_INT || execute_on_SP) {
          if (pI->m_is_cdp && !warp(warp_id).m_cdp_dummy) {
            if (pI->m_is_cdp != 1) { 
              stallData[m_shader->get_sid()][warp_id][others]=1;
              return; }
          }
        }
        else
        {
            stallData[m_shader->get_sid()][warp_id][comp_str]=1;
            //stallData[m_shader->get_sid()][warp_id][mem_data]=1;
        }
      } else if ((m_shader->m_config->gpgpu_num_dp_units > 0) &&
                  (pI->op == DP_OP) &&
                  !(diff_exec_units && previous_issued_inst_exec_type ==
                                          exec_unit_type_t::DP)) {
        if (dp_pipe_avail) {}
        else {
          stallData[m_shader->get_sid()][warp_id][comp_str]=1;
          //stallData[m_shader->get_sid()][warp_id][mem_data]=1;
        }
      }  // If the DP units = 0 (like in Fermi archi), then execute DP
          // inst on SFU unit
      else if (((m_shader->m_config->gpgpu_num_dp_units == 0 &&
                  pI->op == DP_OP) ||
                (pI->op == SFU_OP) || (pI->op == ALU_SFU_OP)) &&
                !(diff_exec_units && previous_issued_inst_exec_type ==
                                        exec_unit_type_t::SFU)) {
        if (sfu_pipe_avail) {}
        else {
          stallData[m_shader->get_sid()][warp_id][comp_str]=1;
          //stallData[m_shader->get_sid()][warp_id][mem_data]=1;
        }
      } else if ((pI->op == TENSOR_CORE_OP) &&
                  !(diff_exec_units && previous_issued_inst_exec_type ==
                                          exec_unit_type_t::TENSOR)) {
        if (tensor_core_pipe_avail) {}
        else {
          stallData[m_shader->get_sid()][warp_id][comp_str]=1;
          //stallData[m_shader->get_sid()][warp_id][mem_data]=1;
        }
      } else if ((pI->op >= SPEC_UNIT_START_ID) &&
                  !(diff_exec_units &&
                    previous_issued_inst_exec_type ==
                        exec_unit_type_t::SPECIALIZED)) {
        unsigned spec_id = pI->op - SPEC_UNIT_START_ID;
        assert(spec_id < m_shader->m_config->m_specialized_unit.size());
        register_set *spec_reg_set = m_spec_cores_out[spec_id];
        bool spec_pipe_avail =
            (m_shader->m_config->m_specialized_unit[spec_id].num_units >
              0) &&
            spec_reg_set->has_free(m_shader->m_config->sub_core_model,
                                    m_id);

        if (spec_pipe_avail) {}
        else {
          stallData[m_shader->get_sid()][warp_id][comp_str]=1;
          //stallData[m_shader->get_sid()][warp_id][mem_data]=1;
        }
      }
    }

    // Get data regarding which structural units are available
    str_status[m_shader->get_sid()][m_id][mem_inst] = (m_mem_out->has_free(m_shader->m_config->sub_core_model, m_id));
    str_status[m_shader->get_sid()][m_id][sp_inst] =  (m_shader->m_config->gpgpu_num_sp_units > 0) && m_sp_out->has_free(m_shader->m_config->sub_core_model, m_id);
    str_status[m_shader->get_sid()][m_id][sfu_inst] =  (m_shader->m_config->gpgpu_num_sfu_units > 0) && m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);
    str_status[m_shader->get_sid()][m_id][tensor_inst] =  (m_shader->m_config->gpgpu_num_tensor_core_units > 0) && m_tensor_core_out->has_free(m_shader->m_config->sub_core_model, m_id);
    str_status[m_shader->get_sid()][m_id][dp_inst] =  (m_shader->m_config->gpgpu_num_dp_units > 0) && m_dp_out->has_free(m_shader->m_config->sub_core_model, m_id);
    str_status[m_shader->get_sid()][m_id][int_inst] =  (m_shader->m_config->gpgpu_num_int_units > 0) && m_int_out->has_free(m_shader->m_config->sub_core_model, m_id);
    str_status[m_shader->get_sid()][m_id][alu_sfu] =  (m_shader->m_config->gpgpu_num_sfu_units > 0) && m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);

  }
  else {
    // this case can happen after a return instruction in diverged warp
    if(!warp(warp_id).ibuffer_empty() && !warp(warp_id).imiss_pending())
      stallData[m_shader->get_sid()][warp_id][control]=1;
  }
}

bool isSync(int inst_opcode)
{
  if((inst_opcode == MMA_OP) || (inst_opcode == MMA_LD_OP) ||
      (inst_opcode == MMA_ST_OP))
      return true;

  return false;
}

void scheduler_unit::cycle(int m_cluster_id) {

  bool mem_data_stall_test = 0;
  bool comp_data_stall_test = 0;
  bool ibuffer_stall_test = 0;
  bool comp_str_stall_test = 0;
  bool mem_str_stall_test = 0;
  bool other_stall_test1 = 0;
  bool other_stall_test2 = 0;
  bool other_stall_test3 = 0;

  bool something_can_be_pushed = false;
  bool consec_inst_indep = false;
  const warp_inst_t *last_exec_inst;

  std::vector<const warp_inst_t *> indep_instructions;
  std::vector<int> indep_warp_num;
  std::vector<int> indep_pc_num;

  SCHED_DPRINTF("scheduler_unit::cycle()\n");
  bool valid_inst =
      false;  // there was one warp with a valid instruction to issue (didn't
              // require flush due to control hazard)
  bool ready_inst = false;   // of the valid instructions, there was one not
                             // waiting for pending register writes
  bool issued_inst = false;  // of these we issued one


  // check if ICNT has less pressure --> Push mem instructions if that is true

  if(icnt_pressure[m_cluster_id] < tot_icnt_buffer && m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp)
  //if(going_from_shader_to_mem <= 7)
  {
    issued_inst = replay_buffer_cycle_mem(m_cluster_id, 1, mem_data_stall_test, comp_data_stall_test, ibuffer_stall_test, comp_str_stall_test ,
  mem_str_stall_test, other_stall_test1, other_stall_test2, other_stall_test3);
  }

  order_warps(m_cluster_id);

  print_on = m_shader->m_config->gpgpu_print_stall_data;

  // Check all warps for possible stalls

  // for (std::vector<shd_warp_t *>::const_iterator iter =
  //     m_next_cycle_prioritized_warps.begin();
  //     iter != m_next_cycle_prioritized_warps.end(); iter++) {
  //   if ((*iter) == NULL) {
  //     continue;
  //   }
  //   if ((*iter)->done_exit()) {
  //     if ((*iter)->get_warp_id() <= 300)
  //     {
  //       stallData[m_shader->get_sid()][(*iter)->get_warp_id()][idlew]=1;
  //       act_warp[m_shader->get_sid()][(*iter)->get_warp_id()] = get_schd_id() + 1;
  //     }
  //     continue;
  //   }
  //   if ((*iter)->get_warp_id() > actw) actw = (*iter)->get_warp_id();
  
  //   act_warp[m_shader->get_sid()][(*iter)->get_warp_id()] = get_schd_id() + 1;
  //   //verify_stall((*iter)->get_warp_id(), exec_unit_type_t::NONE);
  // }

  int counter = 0;

  if(!issued_inst)
  for (std::vector<shd_warp_t *>::const_iterator iter =
           m_next_cycle_prioritized_warps.begin();
       iter != m_next_cycle_prioritized_warps.end(); iter++) {
    // Don't consider warps that are not yet valid
    counter = counter + 1;

    if ((*iter) == NULL || (*iter)->done_exit()) {
      continue;
    }

    something_can_be_pushed = true;

    SCHED_DPRINTF("Testing (warp_id %u, dynamic_warp_id %u)\n",
                  (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
    unsigned warp_id = (*iter)->get_warp_id();
    unsigned checked = 0;
    unsigned issued = 0;
    exec_unit_type_t previous_issued_inst_exec_type = exec_unit_type_t::NONE;
    unsigned max_issue = m_shader->m_config->gpgpu_max_insn_issue_per_warp;
    bool diff_exec_units =
        m_shader->m_config
            ->gpgpu_dual_issue_diff_exec_units;  // In tis mode, we only allow
                                                 // dual issue to diff execution
                                                 // units (as in Maxwell and
                                                 // Pascal)

    if (warp(warp_id).ibuffer_empty())
    {
      SCHED_DPRINTF(
          "Warp (warp_id %u, dynamic_warp_id %u) fails as ibuffer_empty\n",
          (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
      //stallData[m_shader->get_sid()][warp_id][ibufferw]=1;
      stallData[m_shader->get_sid()][warp_id][mem_data]=1;
      ibuffer_stall_test++;
      ibuffer_stall_issue_irr++;
    }

    const warp_inst_t *pI;

    // if(warp(warp_id).check_waiting())
    //   cout <<"WARP_WAITING_N "<<warp_id<<" "<<warp(warp_id).check_waiting()<<"\n"; 

    while (!warp(warp_id).waiting() && !warp(warp_id).ibuffer_empty() &&
           (checked < max_issue) && (checked <= issued) &&
           (issued < max_issue)) {

        // We record the stall reason of the last instruction that
        // could be issued but was stall. We can only issue from one warp
        if (warp_id > actw) actw = warp_id;
    
        act_warp[m_shader->get_sid()][warp_id] = get_schd_id() + 1;
        
        //verify_stall(warp_id, previous_issued_inst_exec_type);

        pI = warp(warp_id).ibuffer_next_inst();
        if(issued == 0)
          last_exec_inst = warp(warp_id).ibuffer_next_inst();
        // Jin: handle cdp latency;
        if (pI && pI->m_is_cdp && warp(warp_id).m_cdp_latency > 0) {
          assert(warp(warp_id).m_cdp_dummy);
          warp(warp_id).m_cdp_latency--;
          stallData[m_shader->get_sid()][warp_id][others]=1;
          other_stall_test1++;
          other_stall_issue_irr1++;
          break;
        }

      bool valid = warp(warp_id).ibuffer_next_valid();
      bool warp_inst_issued = false;
      unsigned pc, rpc;
      m_shader->get_pdom_stack_top_info(warp_id, pI, &pc, &rpc);
      SCHED_DPRINTF(
          "Warp (warp_id %u, dynamic_warp_id %u) has valid instruction (%s)\n",
          (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(),
          m_shader->m_config->gpgpu_ctx->func_sim->ptx_get_insn_str(pc)
              .c_str());

      // dont push if incoming inst is barrier and there are instructions in replay buffer -> 
      // This will ensure all inst in OOO buffer are completed before barrier instruction is executed
      // dont push inst if status of threads is changing in this instruction -> some threads are completing.
      // Finish all pending inst in OOO buffer before threads with different status are going to run 
      // This will take care of threads ending, branches and reconvergence
      
      // if (pI 
      //     && !((pI->op == MEMORY_BARRIER_OP || pI->op == BARRIER_OP ) && !warp(warp_id).replay_buffer_empty() && m_shader->m_config->gpgpu_reply_buffer)
      //     && !(m_shader->checkSIMTthreadStateChange(warp_id,pI) && !warp(warp_id).replay_buffer_empty() && m_shader->m_config->gpgpu_reply_buffer)
      //     && !(m_shader->isSyncInst(pI,int(warp_id)) && !warp(warp_id).replay_buffer_empty() && m_shader->m_config->gpgpu_reply_buffer)
      //   ) {

      if (pI 
          && (!((pI->op == MEMORY_BARRIER_OP || pI->op == BARRIER_OP ) && !warp(warp_id).replay_buffer_empty() && m_shader->m_config->gpgpu_reply_buffer))
          && (!(m_shader->isSyncInst(pI,int(warp_id)) && !warp(warp_id).replay_buffer_empty() && m_shader->m_config->gpgpu_reply_buffer))
          && (!(m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp && m_shader->isSyncInst(pI,int(warp_id)) && !warp(warp_id).replay_buffer_empty_mem()))
          // Addition to prevent memory reordering ->
          // No stores if load in OOO buffer
          //&& (!(pI->is_load() && (warp(warp_id).replay_buffer_next_inst()->is_store() || warp(warp_id).replay_buffer_next_inst()->is_load()) && warp(warp_id).replay_buffer_next_valid() && m_shader->m_config->gpgpu_reply_buffer))
          // No stores issued if load in OOO buffer
          //&& (!(pI->is_store() && (warp(warp_id).replay_buffer_next_inst()->is_load() || warp(warp_id).replay_buffer_next_inst()->is_store()) && warp(warp_id).replay_buffer_next_valid() && m_shader->m_config->gpgpu_reply_buffer))
        ) {

        assert(valid);
        if (pc != pI->pc) {
          SCHED_DPRINTF(
              "Warp (warp_id %u, dynamic_warp_id %u) control hazard "
              "instruction flush\n",
              (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
          // control hazard
          warp(warp_id).set_next_pc(pc);
          warp(warp_id).ibuffer_flush();
          ibuffer_flush_count1++;

          if (m_shader->m_config->perfect_control)
          {
            m_shader->fix_control_hazard(warp_id);
            continue;
          }
        } else 
        {
          valid_inst = true;

          bool bypass_mem = false; 

          std::vector<int> ResMem = (m_scoreboard->checkCollisionMem(warp_id, pI));
          if(ResMem[0]){
            stallData[m_shader->get_sid()][warp_id][mem_data]=1;
            mem_data_stall_test++;
            mem_data_stall_issue_irr++;
          }

          if(issued == 1)
          {
            consec_inst_indep = m_scoreboard->checkConsecutiveInstIndep(pI,last_exec_inst);
          }

          std::vector<int> ResComp = (m_scoreboard->checkCollisionComp(warp_id, pI));
          if(ResComp[0]){
            stallData[m_shader->get_sid()][warp_id][comp_data]=1;
            comp_data_stall_test++;
            comp_data_stall_issue_irr++;
          } 

          // check against replay queue as well if replay queue is enabled

          bool replay_collision = true;
          if(!m_shader->m_config->gpgpu_reply_buffer)
            replay_collision = false;
          else
          {
            std::vector<const warp_inst_t *> replayInst = warp(warp_id).get_Replay_buffer_Inst();
            replay_collision = m_scoreboard->checkReplayCollision(warp_id, pI,replayInst);
          }

          bool replay_collision_mem = true;
          if(!m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp)
            replay_collision_mem = false;
          else
          {
            std::vector<const warp_inst_t *> replayInstMem = warp(warp_id).get_Replay_buffer_Inst_mem();
            replay_collision_mem = m_scoreboard->checkReplayCollision(warp_id, pI,replayInstMem);
          }
          

          if ((!m_scoreboard->checkCollision(warp_id, pI,0) && !replay_collision) || bypass_mem)
          {
            SCHED_DPRINTF(
                "Warp (warp_id %u, dynamic_warp_id %u) passes scoreboard\n",
                (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
            ready_inst = true;

            const active_mask_t &active_mask =
                m_shader->get_active_mask(warp_id, pI);

            assert(warp(warp_id).inst_in_pipeline());

            if ((pI->op == LOAD_OP) || (pI->op == STORE_OP) ||
                (pI->op == MEMORY_BARRIER_OP) ||
                (pI->op == TENSOR_CORE_LOAD_OP) ||
                (pI->op == TENSOR_CORE_STORE_OP)) {
              if (m_mem_out->has_free(m_shader->m_config->sub_core_model,
                                      m_id) &&
                  (!diff_exec_units ||
                   previous_issued_inst_exec_type != exec_unit_type_t::MEM)) {

                // If OOO mem queue not enabled, or OOO mem queue is empty, push inst in this cycle
                // Else put mem instruction in OOO mem queue to be executed later and move the cycle forward
                if(!m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp || warp(warp_id).replay_buffer_empty_mem())
                {
                  m_shader->issue_warp(*m_mem_out, pI, active_mask, warp_id,
                                     m_id, m_shader->get_sid());
                  //cout <<"PUSHING_NORMAL_HERE "<<warp_id<<"\n";
                  mem_inst_issue++;
                  issued++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::MEM;
                }
              }
              else
              {
                stallData[m_shader->get_sid()][warp_id][mem_str]=1;
                mem_str_stall_test++;
                mem_str_stall_issue_irr++;
              }
            }
            // Addition for replay mem check. Dependent mem instructions can be added to OOO mem queue since execution of
            // these instructions will be in order 
            else if(!replay_collision_mem) {
              bool sp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sp_units > 0) &&
                  m_sp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool sfu_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sfu_units > 0) &&
                  m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool tensor_core_pipe_avail =
                  (m_shader->m_config->gpgpu_num_tensor_core_units > 0) &&
                  m_tensor_core_out->has_free(
                      m_shader->m_config->sub_core_model, m_id);
              bool dp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_dp_units > 0) &&
                  m_dp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool int_pipe_avail =
                  (m_shader->m_config->gpgpu_num_int_units > 0) &&
                  m_int_out->has_free(m_shader->m_config->sub_core_model, m_id);

              // This code need to be refactored
              if (pI->op != TENSOR_CORE_OP && pI->op != SFU_OP &&
                  pI->op != DP_OP && !(pI->op >= SPEC_UNIT_START_ID)) {
                bool execute_on_SP = false;
                bool execute_on_INT = false;

                // if INT unit pipline exist, then execute ALU and INT
                // operations on INT unit and SP-FPU on SP unit (like in Volta)
                // if INT unit pipline does not exist, then execute all ALU, INT
                // and SP operations on SP unit (as in Fermi, Pascal GPUs)
                if (m_shader->m_config->gpgpu_num_int_units > 0 &&
                    int_pipe_avail && pI->op != SP_OP &&
                    !(diff_exec_units &&
                      previous_issued_inst_exec_type == exec_unit_type_t::INT))
                  execute_on_INT = true;
                else if (sp_pipe_avail &&
                         (m_shader->m_config->gpgpu_num_int_units == 0 ||
                          (m_shader->m_config->gpgpu_num_int_units > 0 &&
                           pI->op == SP_OP)) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::SP))
                  execute_on_SP = true;

                if (execute_on_INT || execute_on_SP) {
                  // Jin: special for CDP api
                  if (pI->m_is_cdp && !warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);

                    if (pI->m_is_cdp == 1)
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1];
                    else  // cudaLaunchDeviceV2 and cudaGetParameterBufferV2
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1] +
                          m_shader->m_config->gpgpu_ctx->func_sim
                                  ->cdp_latency[pI->m_is_cdp] *
                              active_mask.count();
                    warp(warp_id).m_cdp_dummy = true;
                    stallData[m_shader->get_sid()][warp_id][others]=1;
                    other_stall_test2++;
                    other_stall_issue_irr2++;
                    break;
                  } else if (pI->m_is_cdp && warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);
                    warp(warp_id).m_cdp_dummy = false;
                  }
                }

                if (execute_on_SP) {
                  m_shader->issue_warp(*m_sp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid());
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SP;
                } else if (execute_on_INT) {
                  m_shader->issue_warp(*m_int_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid());
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::INT;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((m_shader->m_config->gpgpu_num_dp_units > 0) &&
                         (pI->op == DP_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::DP)) {
                if (dp_pipe_avail) {
                  m_shader->issue_warp(*m_dp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid());
                  issued++;
                  issued_inst = true;
                  comp_inst_issue++;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::DP;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }  // If the DP units = 0 (like in Fermi archi), then execute DP
                 // inst on SFU unit
              else if (((m_shader->m_config->gpgpu_num_dp_units == 0 &&
                         pI->op == DP_OP) ||
                        (pI->op == SFU_OP) || (pI->op == ALU_SFU_OP)) &&
                       !(diff_exec_units && previous_issued_inst_exec_type ==
                                                exec_unit_type_t::SFU)) {
                if (sfu_pipe_avail) {
                  m_shader->issue_warp(*m_sfu_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid());
                  issued++;
                  issued_inst = true;
                  comp_inst_issue++;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SFU;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op == TENSOR_CORE_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::TENSOR)) {
                if (tensor_core_pipe_avail) {
                  m_shader->issue_warp(*m_tensor_core_out, pI, active_mask,
                                       warp_id, m_id, m_shader->get_sid());
                  issued++;
                  issued_inst = true;
                  comp_inst_issue++;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::TENSOR;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op >= SPEC_UNIT_START_ID) &&
                         !(diff_exec_units &&
                           previous_issued_inst_exec_type ==
                               exec_unit_type_t::SPECIALIZED)) {
                unsigned spec_id = pI->op - SPEC_UNIT_START_ID;
                assert(spec_id < m_shader->m_config->m_specialized_unit.size());
                register_set *spec_reg_set = m_spec_cores_out[spec_id];
                bool spec_pipe_avail =
                    (m_shader->m_config->m_specialized_unit[spec_id].num_units >
                     0) &&
                    spec_reg_set->has_free(m_shader->m_config->sub_core_model,
                                           m_id);

                if (spec_pipe_avail) {
                  m_shader->issue_warp(*spec_reg_set, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid());
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type =
                      exec_unit_type_t::SPECIALIZED;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }

            }  // end of else
          } else {
          }

          // synchronization instructions should not go in OOO replay buffer
          //if(!issued_inst && pI && !warp(warp_id).ibuffer_empty() && m_shader->m_config->gpgpu_reply_buffer && !m_shader->isSyncInst(pI,int(warp_id)))

          if(!issued_inst && pI && (warp(warp_id).ibuffer_count() > 1) && m_shader->m_config->gpgpu_reply_buffer && !m_shader->isSyncInst(pI,int(warp_id)))
          {
            bool replay_collision_mem = false;
            bool replay_collision_ooo = false;
            if(m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp)
            {
              if(!((pI->op == LOAD_OP) || (pI->op == STORE_OP) || (pI->op == MEMORY_BARRIER_OP) || (pI->op == TENSOR_CORE_LOAD_OP) || (pI->op == TENSOR_CORE_STORE_OP)))
              {
                std::vector<const warp_inst_t *> replayInstMem = warp(warp_id).get_Replay_buffer_Inst_mem();
                replay_collision_mem = m_scoreboard->checkReplayCollision(warp_id, pI,replayInstMem);
              }
              else
              {
                std::vector<const warp_inst_t *> replayInst = warp(warp_id).get_Replay_buffer_Inst();
                replay_collision_ooo = m_scoreboard->checkReplayCollision(warp_id, pI,replayInst);
              }
            }

            if(!replay_collision_mem && !replay_collision_ooo)
            {
              last_exec_inst = warp(warp_id).ibuffer_check_indep_inst();
              consec_inst_indep = m_scoreboard->checkConsecutiveInstIndep(pI,last_exec_inst);
              if(consec_inst_indep)
              {
                indep_instructions.push_back(pI);
                indep_warp_num.push_back(warp_id);
                indep_pc_num.push_back(pc);

                if((pI->op == LOAD_OP) || (pI->op == STORE_OP) || (pI->op == MEMORY_BARRIER_OP) || (pI->op == TENSOR_CORE_LOAD_OP) || (pI->op == TENSOR_CORE_STORE_OP))
                  opp_for_mem++;
              }
            }
          }
        }
      } else if (valid && !pI) {

        // this case can happen after a return instruction in diverged warp
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) return from diverged warp "
            "flush\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
        warp(warp_id).set_next_pc(pc);
        warp(warp_id).ibuffer_flush();
        ibuffer_flush_count2++;
        if (m_shader->m_config->perfect_control)
        {
          m_shader->fix_control_hazard(warp_id);
          continue;
        }
        stallData[m_shader->get_sid()][warp_id][others]=1;
        other_stall_test3++;
        other_stall_issue_irr3++;
      }
      if (warp_inst_issued) {
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) issued %u instructions\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(), issued);
        do_on_warp_issued(warp_id, issued, iter);
      }
      checked++;
      }
      if (issued) {

        warp_issue[warp_id]++;
        if(m_shader->m_config->gpgpu_write_sched_order)
          write_warps << warp_id <<" ";

        //cout <<"ISSUING_NORMAL_INSTRUCTION_HERE\n";

        issued_inst_count++;

        // This might be a bit inefficient, but we need to maintain
        // two ordered list for proper scheduler execution.
        // We could remove the need for this loop by associating a
        // supervised_is index with each entry in the
        // m_next_cycle_prioritized_warps vector. For now, just run through until
        // you find the right warp_id
        for (std::vector<shd_warp_t *>::const_iterator supervised_iter =
                m_supervised_warps.begin();
            supervised_iter != m_supervised_warps.end(); ++supervised_iter) {
          if (*iter == *supervised_iter) {
            m_last_supervised_issued = supervised_iter;
          }
        }

      // if(!warp(warp_id).ibuffer_empty())
      // {
      //   last_exec_inst = warp(warp_id).ibuffer_next_inst();
      //   consec_inst_indep = m_scoreboard->checkConsecutiveInstIndep(pI,last_exec_inst);
      //   if(consec_inst_indep)
      //   {
      //     indep_consec_inst++;
      //   }
      // }

      if (issued == 1)
      {
        m_stats->single_issue_nums[m_id]++;
      }
      else if (issued > 1)
      {
        m_stats->dual_issue_nums[m_id]++;
      }
      else
        abort();  // issued should be > 0
      break;
    }
    else{
    NO_INST_ISSUE++;
    }
  }

  //replay instructions is enabled and no instructions were issued in this cycle
  if(m_shader->m_config->gpgpu_reply_buffer && !issued_inst)
  {
    //replay instructions in pipeline already
    issued_inst = replay_buffer_cycle(m_cluster_id, 0, mem_data_stall_test, comp_data_stall_test, ibuffer_stall_test, comp_str_stall_test ,
   mem_str_stall_test, other_stall_test1, other_stall_test2, other_stall_test3);
  }

  if(m_shader->m_config->gpgpu_reply_buffer)
  {
    // add new independent instructions for replay later 
    for (int i = 0; i< indep_warp_num.size(); i++)
    {
      const warp_inst_t *pI;
      pI = indep_instructions[i];
      if(((pI->op == LOAD_OP) || (pI->op == STORE_OP) || (pI->op == MEMORY_BARRIER_OP) || (pI->op == TENSOR_CORE_LOAD_OP) || (pI->op == TENSOR_CORE_STORE_OP))
      && m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp
      )
      {
        int id = m_shader->m_warp[indep_warp_num[i]]->replay_buffer_empty_idx_mem();
        if(id != -1)
        {
            int warp_num = indep_warp_num[i];
            int pc_num = indep_pc_num[i];
            //m_shader->m_warp[warp_num]->replay_buffer_fill(id,pI,pc_num);
            const active_mask_t &active_mask = m_shader->get_active_mask(warp_num, pI);
            m_shader->issue_warp_push_in_replay(*m_mem_out, pI, active_mask, warp_num, m_id, id, pc_num, m_shader->get_sid(),1);
            opp_for_ooo++;
        }
      }
      else
      {
        int id = m_shader->m_warp[indep_warp_num[i]]->replay_buffer_empty_idx();
        if(id != -1)
        {
            int warp_num = indep_warp_num[i];
            int pc_num = indep_pc_num[i];
            opp_for_ooo++;
            //m_shader->m_warp[warp_num]->replay_buffer_fill(id,pI,pc_num);
            const active_mask_t &active_mask = m_shader->get_active_mask(warp_num, pI);
            m_shader->issue_warp_push_in_replay(*m_mem_out, pI, active_mask, warp_num, m_id, id, pc_num, m_shader->get_sid(),0);
        }
      }
    }
  }

  if(icnt_pressure[m_cluster_id] >= tot_icnt_buffer && m_shader->m_config->gpgpu_reply_buffer_seperate_mem_comp && !issued_inst)
  //if(going_from_shader_to_mem <= 7)
  {
    issued_inst = replay_buffer_cycle_mem(m_cluster_id, 1, mem_data_stall_test, comp_data_stall_test, ibuffer_stall_test, comp_str_stall_test ,
  mem_str_stall_test, other_stall_test1, other_stall_test2, other_stall_test3);
  }

  // issue stall statistics:
  if (!valid_inst)
    m_stats->shader_cycle_distro[0]++;  // idle or control hazard
  else if (!ready_inst)
    m_stats->shader_cycle_distro[1]++;  // waiting for RAW hazards (possibly due
                                        // to memory)
  else if (!issued_inst)
  {
    m_stats->shader_cycle_distro[2]++;  // pipeline stalled
    stall_cycles++;
  }

  //if(issued_inst)
  {
    tot_cycles_exec_all_SM++;
  }

  if(issued_inst)
  {
    tot_inst_exec++;
  }

  if(mem_data_stall_test>0 && !issued_inst)
  {
    mem_data_stall++;
  }

  if(comp_data_stall_test>0 && !issued_inst)
  {
    comp_data_stall++;
  }

  if(ibuffer_stall_test>0 && !issued_inst)
  {
    ibuffer_stall++;
  }

  if(mem_str_stall_test>0 && !issued_inst)
  {
    mem_str_stall++;
  }

  if(other_stall_test1>0 && !issued_inst)
  {
    other_stall1++;
  }

  if(other_stall_test2>0 && !issued_inst)
  {
    other_stall2++;
  }

  if(other_stall_test3>0 && !issued_inst)
  {
    other_stall3++;
  }

}

bool scheduler_unit::replay_buffer_cycle_mem(int m_cluster_id, int MEM_ON, int mem_data_stall_test, int comp_data_stall_test, int ibuffer_stall_test, int comp_str_stall_test ,
  int mem_str_stall_test, int other_stall_test1, int other_stall_test2, int other_stall_test3) {

  bool something_can_be_pushed = false;
  bool consec_inst_indep = false;
  const warp_inst_t *last_exec_inst;

  SCHED_DPRINTF("scheduler_unit::cycle()\n");
  bool valid_inst =
      false;  // there was one warp with a valid instruction to issue (didn't
              // require flush due to control hazard)
  bool ready_inst = false;   // of the valid instructions, there was one not
                             // waiting for pending register writes
  bool issued_inst = false;  // of these we issued one

  replay_order_warps(m_cluster_id, MEM_ON);

  print_on = m_shader->m_config->gpgpu_print_stall_data;

  int counter = 0;

  for (std::vector<shd_warp_t *>::const_iterator iter =
           m_next_cycle_prioritized_warps.begin();
       iter != m_next_cycle_prioritized_warps.end(); iter++) {
    // Don't consider warps that are not yet valid
    counter = counter + 1;

    if ((*iter) == NULL || (*iter)->done_exit()) {
      continue;
    }

    something_can_be_pushed = true;

    SCHED_DPRINTF("Testing (warp_id %u, dynamic_warp_id %u)\n",
                  (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
    unsigned warp_id = (*iter)->get_warp_id();
    unsigned checked = 0;
    unsigned issued = 0;
    exec_unit_type_t previous_issued_inst_exec_type = exec_unit_type_t::NONE;
    unsigned max_issue = m_shader->m_config->gpgpu_max_insn_issue_per_warp;
    bool diff_exec_units =
        m_shader->m_config
            ->gpgpu_dual_issue_diff_exec_units;  // In tis mode, we only allow
                                                 // dual issue to diff execution
                                                 // units (as in Maxwell and
                                                 // Pascal)
    const warp_inst_t *pI;

    // if(warp(warp_id).check_waiting())
    //   cout <<"WARP_WAITING_M "<<warp_id<<" "<<warp(warp_id).check_waiting()<<"\n"; 

    while (
            !warp(warp_id).waiting() && 
            !warp(warp_id).replay_buffer_empty_mem() &&
           (checked < max_issue) && (checked <= issued) &&
           (issued < max_issue)) {

        // We record the stall reason of the last instruction that
        // could be issued but was stall. We can only issue from one warp
        if (warp_id > actw) actw = warp_id;
    
        act_warp[m_shader->get_sid()][warp_id] = get_schd_id() + 1;
        
        //verify_stall(warp_id, previous_issued_inst_exec_type);

        pI = warp(warp_id).replay_buffer_next_inst_mem();
        if(issued == 0)
          last_exec_inst = warp(warp_id).replay_buffer_next_inst_mem();
        // Jin: handle cdp latency;
        if (pI && pI->m_is_cdp && warp(warp_id).m_cdp_latency > 0) {
          assert(warp(warp_id).m_cdp_dummy);
          warp(warp_id).m_cdp_latency--;
          stallData[m_shader->get_sid()][warp_id][others]=1;
          other_stall_test1++;
          other_stall_issue_irr1++;
          break;
        }

      bool valid = warp(warp_id).replay_buffer_next_valid_mem();
      bool warp_inst_issued = false;
      unsigned pc, rpc;
      // m_shader->get_pdom_stack_top_info(warp_id, pI, &pc, &rpc);
      pc = warp(warp_id).replay_buffer_next_pc_mem();

      SCHED_DPRINTF(
          "Warp (warp_id %u, dynamic_warp_id %u) has valid instruction (%s)\n",
          (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(),
          m_shader->m_config->gpgpu_ctx->func_sim->ptx_get_insn_str(pc)
              .c_str());
      if (pI) {

        assert(valid);
        // if (pc != pI->pc) {
        //   SCHED_DPRINTF(
        //       "Warp (warp_id %u, dynamic_warp_id %u) control hazard "
        //       "instruction flush\n",
        //       (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
        //   // control hazard
        //   warp(warp_id).set_next_pc(pc);
        //   warp(warp_id).ibuffer_flush();
        //   if (m_shader->m_config->perfect_control)
        //   {
        //     m_shader->fix_control_hazard(warp_id);
        //     continue;
        //   }
        // } else 
        {
          valid_inst = true;

          bool bypass_mem = false; 

          std::vector<int> ResMem = (m_scoreboard->checkCollisionMem(warp_id, pI));
          if(ResMem[0]){
            stallData[m_shader->get_sid()][warp_id][mem_data]=1;
            mem_data_stall_test++;
            mem_data_stall_issue_irr++;
          }

          if(issued == 1)
          {
            consec_inst_indep = m_scoreboard->checkConsecutiveInstIndep(pI,last_exec_inst);
          }

          std::vector<int> ResComp = (m_scoreboard->checkCollisionComp(warp_id, pI));
          if(ResComp[0]){
            stallData[m_shader->get_sid()][warp_id][comp_data]=1;
            comp_data_stall_test++;
            comp_data_stall_issue_irr++;
          }

          if (!m_scoreboard->checkCollision(warp_id, pI,0) || bypass_mem) 
          {

            SCHED_DPRINTF(
                "Warp (warp_id %u, dynamic_warp_id %u) passes scoreboard\n",
                (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
            ready_inst = true;

            const active_mask_t &active_mask =
                m_shader->get_active_mask(warp_id, pI);

            assert(warp(warp_id).inst_in_pipeline());

            if ((pI->op == LOAD_OP) || (pI->op == STORE_OP) ||
                (pI->op == MEMORY_BARRIER_OP) ||
                (pI->op == TENSOR_CORE_LOAD_OP) ||
                (pI->op == TENSOR_CORE_STORE_OP)) {
              if (m_mem_out->has_free(m_shader->m_config->sub_core_model,
                                      m_id) &&
                  (!diff_exec_units ||
                   previous_issued_inst_exec_type != exec_unit_type_t::MEM)) {

                //m_shader->issue_warp_push_from_replay_mem(*m_mem_out, pI, active_mask, warp_id,
                //                     m_id, m_shader->get_sid(),MEM_ON);
                m_shader->issue_warp_push_from_replay(*m_mem_out, pI, active_mask, warp_id,
                                    m_id, m_shader->get_sid(),MEM_ON);
                issued++;
                mem_inst_issue++;
                issued_inst = true;
                warp_inst_issued = true;
                previous_issued_inst_exec_type = exec_unit_type_t::MEM;
              }
              else
              {
                stallData[m_shader->get_sid()][warp_id][mem_str]=1;
                mem_str_stall_test++;
                mem_str_stall_issue_irr++;
              }
            } else {
              bool sp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sp_units > 0) &&
                  m_sp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool sfu_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sfu_units > 0) &&
                  m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool tensor_core_pipe_avail =
                  (m_shader->m_config->gpgpu_num_tensor_core_units > 0) &&
                  m_tensor_core_out->has_free(
                      m_shader->m_config->sub_core_model, m_id);
              bool dp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_dp_units > 0) &&
                  m_dp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool int_pipe_avail =
                  (m_shader->m_config->gpgpu_num_int_units > 0) &&
                  m_int_out->has_free(m_shader->m_config->sub_core_model, m_id);

              // This code need to be refactored
              if (pI->op != TENSOR_CORE_OP && pI->op != SFU_OP &&
                  pI->op != DP_OP && !(pI->op >= SPEC_UNIT_START_ID)) {
                bool execute_on_SP = false;
                bool execute_on_INT = false;

                // if INT unit pipline exist, then execute ALU and INT
                // operations on INT unit and SP-FPU on SP unit (like in Volta)
                // if INT unit pipline does not exist, then execute all ALU, INT
                // and SP operations on SP unit (as in Fermi, Pascal GPUs)
                if (m_shader->m_config->gpgpu_num_int_units > 0 &&
                    int_pipe_avail && pI->op != SP_OP &&
                    !(diff_exec_units &&
                      previous_issued_inst_exec_type == exec_unit_type_t::INT))
                  execute_on_INT = true;
                else if (sp_pipe_avail &&
                         (m_shader->m_config->gpgpu_num_int_units == 0 ||
                          (m_shader->m_config->gpgpu_num_int_units > 0 &&
                           pI->op == SP_OP)) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::SP))
                  execute_on_SP = true;

                if (execute_on_INT || execute_on_SP) {
                  // Jin: special for CDP api
                  if (pI->m_is_cdp && !warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);

                    if (pI->m_is_cdp == 1)
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1];
                    else  // cudaLaunchDeviceV2 and cudaGetParameterBufferV2
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1] +
                          m_shader->m_config->gpgpu_ctx->func_sim
                                  ->cdp_latency[pI->m_is_cdp] *
                              active_mask.count();
                    warp(warp_id).m_cdp_dummy = true;
                    stallData[m_shader->get_sid()][warp_id][others]=1;
                    other_stall_test2++;
                    other_stall_issue_irr2++;
                    break;
                  } else if (pI->m_is_cdp && warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);
                    warp(warp_id).m_cdp_dummy = false;
                  }
                }

                if (execute_on_SP) {
                  // m_shader->issue_warp_push_from_replay_mem(*m_sp_out, pI, active_mask, warp_id,
                  //                      m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay_mem(*m_sp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(), MEM_ON);
                  issued++;
                  issued_inst = true;
                  comp_inst_issue++;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SP;
                } else if (execute_on_INT) {
                  // m_shader->issue_warp_push_from_replay_mem(*m_int_out, pI, active_mask, warp_id,
                  //                      m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay(*m_int_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::INT;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((m_shader->m_config->gpgpu_num_dp_units > 0) &&
                         (pI->op == DP_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::DP)) {
                if (dp_pipe_avail) {
                  // m_shader->issue_warp_push_from_replay_mem(*m_dp_out, pI, active_mask, warp_id,
                  //                      m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay(*m_dp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::DP;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }  // If the DP units = 0 (like in Fermi archi), then execute DP
                 // inst on SFU unit
              else if (((m_shader->m_config->gpgpu_num_dp_units == 0 &&
                         pI->op == DP_OP) ||
                        (pI->op == SFU_OP) || (pI->op == ALU_SFU_OP)) &&
                       !(diff_exec_units && previous_issued_inst_exec_type ==
                                                exec_unit_type_t::SFU)) {
                if (sfu_pipe_avail) {
                  // m_shader->issue_warp_push_from_replay_mem(*m_sfu_out, pI, active_mask, warp_id,
                  //                      m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay(*m_sfu_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SFU;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op == TENSOR_CORE_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::TENSOR)) {
                if (tensor_core_pipe_avail) {
                  // m_shader->issue_warp_push_from_replay_mem(*m_tensor_core_out, pI, active_mask,
                  //                      warp_id, m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay(*m_tensor_core_out, pI, active_mask,
                                       warp_id, m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::TENSOR;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op >= SPEC_UNIT_START_ID) &&
                         !(diff_exec_units &&
                           previous_issued_inst_exec_type ==
                               exec_unit_type_t::SPECIALIZED)) {
                unsigned spec_id = pI->op - SPEC_UNIT_START_ID;
                assert(spec_id < m_shader->m_config->m_specialized_unit.size());
                register_set *spec_reg_set = m_spec_cores_out[spec_id];
                bool spec_pipe_avail =
                    (m_shader->m_config->m_specialized_unit[spec_id].num_units >
                     0) &&
                    spec_reg_set->has_free(m_shader->m_config->sub_core_model,
                                           m_id);

                if (spec_pipe_avail) {
                  // m_shader->issue_warp_push_from_replay_mem(*spec_reg_set, pI, active_mask, warp_id,
                  //                      m_id, m_shader->get_sid());
                  m_shader->issue_warp_push_from_replay(*spec_reg_set, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type =
                      exec_unit_type_t::SPECIALIZED;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }

            }  // end of else
          } else {
            SCHED_DPRINTF(
                "Warp (warp_id %u, dynamic_warp_id %u) fails scoreboard\n",
                (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
          }
        }
      } else if (valid) {
        // this case can happen after a return instruction in diverged warp
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) return from diverged warp "
            "flush\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
        warp(warp_id).set_next_pc(pc);
        warp(warp_id).replay_buffer_flush_mem();
        if (m_shader->m_config->perfect_control)
        {
          m_shader->fix_control_hazard(warp_id);
          continue;
        }
        stallData[m_shader->get_sid()][warp_id][others]=1;
        other_stall_test3++;
        other_stall_issue_irr3++;
      }
      if (warp_inst_issued) {
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) issued %u instructions\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(), issued);
        replay_do_on_warp_issued_mem(warp_id, issued, iter,MEM_ON);
      }
      checked++;
    }
    if (issued) {
      warp_issue[warp_id]++;

      issued_inst_count++;

      //cout <<"ISSUING_MEM_INSTRUCTION_HERE\n";


      if(m_shader->m_config->gpgpu_write_sched_order)
        write_warps << warp_id <<" ";

      // This might be a bit inefficient, but we need to maintain
      // two ordered list for proper scheduler execution.
      // We could remove the need for this loop by associating a
      // supervised_is index with each entry in the
      // m_next_cycle_prioritized_warps vector. For now, just run through until
      // you find the right warp_id
      for (std::vector<shd_warp_t *>::const_iterator supervised_iter =
               m_supervised_warps.begin();
           supervised_iter != m_supervised_warps.end(); ++supervised_iter) {
        if (*iter == *supervised_iter) {
          m_last_supervised_issued = supervised_iter;
        }
      }

      if (issued == 1)
      {
        m_stats->single_issue_nums[m_id]++;
      }
      else if (issued > 1)
      {
        m_stats->dual_issue_nums[m_id]++;
      }
      else
        abort();  // issued should be > 0

      break;
    }
  }

  // issue stall statistics:
  if (!valid_inst)
    m_stats->shader_cycle_distro[0]++;  // idle or control hazard
  else if (!ready_inst)
    m_stats->shader_cycle_distro[1]++;  // waiting for RAW hazards (possibly due
                                        // to memory)
  else if (!issued_inst)
  {
    m_stats->shader_cycle_distro[2]++;  // pipeline stalled
  }
  return issued_inst;
}

// MEM ON STATES IF MEMORY IS DONE OOO OR COMPUTE
bool scheduler_unit::replay_buffer_cycle(int m_cluster_id, int MEM_ON, int mem_data_stall_test, int comp_data_stall_test, int ibuffer_stall_test, int comp_str_stall_test ,
  int mem_str_stall_test, int other_stall_test1, int other_stall_test2, int other_stall_test3) {

  bool something_can_be_pushed = false;
  bool consec_inst_indep = false;
  const warp_inst_t *last_exec_inst;

  SCHED_DPRINTF("scheduler_unit::cycle()\n");
  bool valid_inst =
      false;  // there was one warp with a valid instruction to issue (didn't
              // require flush due to control hazard)
  bool ready_inst = false;   // of the valid instructions, there was one not
                             // waiting for pending register writes
  bool issued_inst = false;  // of these we issued one

  replay_order_warps(m_cluster_id, MEM_ON);

  print_on = m_shader->m_config->gpgpu_print_stall_data;

  int counter = 0;

  for (std::vector<shd_warp_t *>::const_iterator iter =
           m_next_cycle_prioritized_warps.begin();
       iter != m_next_cycle_prioritized_warps.end(); iter++) {
    // Don't consider warps that are not yet valid
    counter = counter + 1;

    if ((*iter) == NULL || (*iter)->done_exit()) {
      continue;
    }

    something_can_be_pushed = true;

    SCHED_DPRINTF("Testing (warp_id %u, dynamic_warp_id %u)\n",
                  (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
    unsigned warp_id = (*iter)->get_warp_id();
    unsigned checked = 0;
    unsigned issued = 0;
    exec_unit_type_t previous_issued_inst_exec_type = exec_unit_type_t::NONE;
    unsigned max_issue = m_shader->m_config->gpgpu_max_insn_issue_per_warp;
    bool diff_exec_units =
        m_shader->m_config
            ->gpgpu_dual_issue_diff_exec_units;  // In tis mode, we only allow
                                                 // dual issue to diff execution
                                                 // units (as in Maxwell and
                                                 // Pascal)
    const warp_inst_t *pI;

    while (
            !warp(warp_id).waiting() && 
            !warp(warp_id).replay_buffer_empty() &&
           (checked < max_issue) && (checked <= issued) &&
           (issued < max_issue)) {

        // We record the stall reason of the last instruction that
        // could be issued but was stall. We can only issue from one warp
        if (warp_id > actw) actw = warp_id;
    
        act_warp[m_shader->get_sid()][warp_id] = get_schd_id() + 1;
        
        //verify_stall(warp_id, previous_issued_inst_exec_type);

        pI = warp(warp_id).replay_buffer_next_inst();
        if(issued == 0)
          last_exec_inst = warp(warp_id).replay_buffer_next_inst();
        // Jin: handle cdp latency;
        if (pI && pI->m_is_cdp && warp(warp_id).m_cdp_latency > 0) {
          assert(warp(warp_id).m_cdp_dummy);
          warp(warp_id).m_cdp_latency--;
          stallData[m_shader->get_sid()][warp_id][others]=1;
          other_stall_test1++;
          other_stall_issue_irr1++;
          break;
        }

      bool valid = warp(warp_id).replay_buffer_next_valid();
      bool warp_inst_issued = false;
      unsigned pc, rpc;
      // m_shader->get_pdom_stack_top_info(warp_id, pI, &pc, &rpc);
      pc = warp(warp_id).replay_buffer_next_pc();

      SCHED_DPRINTF(
          "Warp (warp_id %u, dynamic_warp_id %u) has valid instruction (%s)\n",
          (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(),
          m_shader->m_config->gpgpu_ctx->func_sim->ptx_get_insn_str(pc)
              .c_str());

      if (pI) {

        assert(valid);
        // if (pc != pI->pc) {
        //   SCHED_DPRINTF(
        //       "Warp (warp_id %u, dynamic_warp_id %u) control hazard "
        //       "instruction flush\n",
        //       (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
        //   // control hazard
        //   warp(warp_id).set_next_pc(pc);
        //   warp(warp_id).ibuffer_flush();
        //   if (m_shader->m_config->perfect_control)
        //   {
        //     m_shader->fix_control_hazard(warp_id);
        //     continue;
        //   }
        // } else 
        {
          valid_inst = true;

          bool bypass_mem = false; 

          std::vector<int> ResMem = (m_scoreboard->checkCollisionMem(warp_id, pI));
          if(ResMem[0]){
            stallData[m_shader->get_sid()][warp_id][mem_data]=1;
            mem_data_stall_test++;
            mem_data_stall_issue_irr++;
          }

          if(issued == 1)
          {
            consec_inst_indep = m_scoreboard->checkConsecutiveInstIndep(pI,last_exec_inst);
          }

          std::vector<int> ResComp = (m_scoreboard->checkCollisionComp(warp_id, pI));
          if(ResComp[0]){
            stallData[m_shader->get_sid()][warp_id][comp_data]=1;
            comp_data_stall_test++;
            comp_data_stall_issue_irr++;
          }

          if (!m_scoreboard->checkCollision(warp_id, pI,0) || bypass_mem) 
          {

            SCHED_DPRINTF(
                "Warp (warp_id %u, dynamic_warp_id %u) passes scoreboard\n",
                (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
            ready_inst = true;

            const active_mask_t &active_mask =
                m_shader->get_active_mask(warp_id, pI);

            assert(warp(warp_id).inst_in_pipeline());

            if ((pI->op == LOAD_OP) || (pI->op == STORE_OP) ||
                (pI->op == MEMORY_BARRIER_OP) ||
                (pI->op == TENSOR_CORE_LOAD_OP) ||
                (pI->op == TENSOR_CORE_STORE_OP)) {
              if (m_mem_out->has_free(m_shader->m_config->sub_core_model,
                                      m_id) &&
                  (!diff_exec_units ||
                   previous_issued_inst_exec_type != exec_unit_type_t::MEM)) {

                m_shader->issue_warp_push_from_replay(*m_mem_out, pI, active_mask, warp_id,
                                     m_id, m_shader->get_sid(),MEM_ON);
                issued++;
                mem_inst_issue++;
                issued_inst = true;
                warp_inst_issued = true;
                previous_issued_inst_exec_type = exec_unit_type_t::MEM;
              }
              else
              {
                stallData[m_shader->get_sid()][warp_id][mem_str]=1;
                mem_str_stall_test++;
                mem_str_stall_issue_irr++;
              }
            } else {
              bool sp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sp_units > 0) &&
                  m_sp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool sfu_pipe_avail =
                  (m_shader->m_config->gpgpu_num_sfu_units > 0) &&
                  m_sfu_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool tensor_core_pipe_avail =
                  (m_shader->m_config->gpgpu_num_tensor_core_units > 0) &&
                  m_tensor_core_out->has_free(
                      m_shader->m_config->sub_core_model, m_id);
              bool dp_pipe_avail =
                  (m_shader->m_config->gpgpu_num_dp_units > 0) &&
                  m_dp_out->has_free(m_shader->m_config->sub_core_model, m_id);
              bool int_pipe_avail =
                  (m_shader->m_config->gpgpu_num_int_units > 0) &&
                  m_int_out->has_free(m_shader->m_config->sub_core_model, m_id);

              // This code need to be refactored
              if (pI->op != TENSOR_CORE_OP && pI->op != SFU_OP &&
                  pI->op != DP_OP && !(pI->op >= SPEC_UNIT_START_ID)) {
                bool execute_on_SP = false;
                bool execute_on_INT = false;

                // if INT unit pipline exist, then execute ALU and INT
                // operations on INT unit and SP-FPU on SP unit (like in Volta)
                // if INT unit pipline does not exist, then execute all ALU, INT
                // and SP operations on SP unit (as in Fermi, Pascal GPUs)
                if (m_shader->m_config->gpgpu_num_int_units > 0 &&
                    int_pipe_avail && pI->op != SP_OP &&
                    !(diff_exec_units &&
                      previous_issued_inst_exec_type == exec_unit_type_t::INT))
                  execute_on_INT = true;
                else if (sp_pipe_avail &&
                         (m_shader->m_config->gpgpu_num_int_units == 0 ||
                          (m_shader->m_config->gpgpu_num_int_units > 0 &&
                           pI->op == SP_OP)) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::SP))
                  execute_on_SP = true;

                if (execute_on_INT || execute_on_SP) {
                  // Jin: special for CDP api
                  if (pI->m_is_cdp && !warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);

                    if (pI->m_is_cdp == 1)
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1];
                    else  // cudaLaunchDeviceV2 and cudaGetParameterBufferV2
                      warp(warp_id).m_cdp_latency =
                          m_shader->m_config->gpgpu_ctx->func_sim
                              ->cdp_latency[pI->m_is_cdp - 1] +
                          m_shader->m_config->gpgpu_ctx->func_sim
                                  ->cdp_latency[pI->m_is_cdp] *
                              active_mask.count();
                    warp(warp_id).m_cdp_dummy = true;
                    stallData[m_shader->get_sid()][warp_id][others]=1;
                    other_stall_test2++;
                    other_stall_issue_irr2++;
                    break;
                  } else if (pI->m_is_cdp && warp(warp_id).m_cdp_dummy) {
                    assert(warp(warp_id).m_cdp_latency == 0);
                    warp(warp_id).m_cdp_dummy = false;
                  }
                }

                if (execute_on_SP) {
                  m_shader->issue_warp_push_from_replay(*m_sp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  issued_inst = true;
                  comp_inst_issue++;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SP;
                } else if (execute_on_INT) {
                  m_shader->issue_warp_push_from_replay(*m_int_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::INT;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((m_shader->m_config->gpgpu_num_dp_units > 0) &&
                         (pI->op == DP_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::DP)) {
                if (dp_pipe_avail) {
                  m_shader->issue_warp_push_from_replay(*m_dp_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::DP;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }  // If the DP units = 0 (like in Fermi archi), then execute DP
                 // inst on SFU unit
              else if (((m_shader->m_config->gpgpu_num_dp_units == 0 &&
                         pI->op == DP_OP) ||
                        (pI->op == SFU_OP) || (pI->op == ALU_SFU_OP)) &&
                       !(diff_exec_units && previous_issued_inst_exec_type ==
                                                exec_unit_type_t::SFU)) {
                if (sfu_pipe_avail) {
                  m_shader->issue_warp_push_from_replay(*m_sfu_out, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::SFU;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op == TENSOR_CORE_OP) &&
                         !(diff_exec_units && previous_issued_inst_exec_type ==
                                                  exec_unit_type_t::TENSOR)) {
                if (tensor_core_pipe_avail) {
                  m_shader->issue_warp_push_from_replay(*m_tensor_core_out, pI, active_mask,
                                       warp_id, m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type = exec_unit_type_t::TENSOR;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              } else if ((pI->op >= SPEC_UNIT_START_ID) &&
                         !(diff_exec_units &&
                           previous_issued_inst_exec_type ==
                               exec_unit_type_t::SPECIALIZED)) {
                unsigned spec_id = pI->op - SPEC_UNIT_START_ID;
                assert(spec_id < m_shader->m_config->m_specialized_unit.size());
                register_set *spec_reg_set = m_spec_cores_out[spec_id];
                bool spec_pipe_avail =
                    (m_shader->m_config->m_specialized_unit[spec_id].num_units >
                     0) &&
                    spec_reg_set->has_free(m_shader->m_config->sub_core_model,
                                           m_id);

                if (spec_pipe_avail) {
                  m_shader->issue_warp_push_from_replay(*spec_reg_set, pI, active_mask, warp_id,
                                       m_id, m_shader->get_sid(),MEM_ON);
                  issued++;
                  comp_inst_issue++;
                  issued_inst = true;
                  warp_inst_issued = true;
                  previous_issued_inst_exec_type =
                      exec_unit_type_t::SPECIALIZED;
                }
                else
                {
                  comp_str_stall_test++;
                  comp_str_stall_issue_irr++;
                }
              }

            }  // end of else
          } else {
            SCHED_DPRINTF(
                "Warp (warp_id %u, dynamic_warp_id %u) fails scoreboard\n",
                (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
          }
        }
      } else if (valid) {
        // this case can happen after a return instruction in diverged warp
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) return from diverged warp "
            "flush\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
        warp(warp_id).set_next_pc(pc);
        warp(warp_id).replay_buffer_flush();
        replay_flush_count++;
        if (m_shader->m_config->perfect_control)
        {
          m_shader->fix_control_hazard(warp_id);
          continue;
        }
        stallData[m_shader->get_sid()][warp_id][others]=1;
        other_stall_test3++;
        other_stall_issue_irr3++;
      }
      if (warp_inst_issued) {
        SCHED_DPRINTF(
            "Warp (warp_id %u, dynamic_warp_id %u) issued %u instructions\n",
            (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id(), issued);
        replay_do_on_warp_issued(warp_id, issued, iter,MEM_ON);
      }
      checked++;
    }
    if (issued) {
      warp_issue[warp_id]++;

      //cout <<"ISSUING_OOO_INSTRUCTION_HERE\n";

      issued_inst_count++;

      if(m_shader->m_config->gpgpu_write_sched_order)
        write_warps << warp_id <<" ";

      // This might be a bit inefficient, but we need to maintain
      // two ordered list for proper scheduler execution.
      // We could remove the need for this loop by associating a
      // supervised_is index with each entry in the
      // m_next_cycle_prioritized_warps vector. For now, just run through until
      // you find the right warp_id
      for (std::vector<shd_warp_t *>::const_iterator supervised_iter =
               m_supervised_warps.begin();
           supervised_iter != m_supervised_warps.end(); ++supervised_iter) {
        if (*iter == *supervised_iter) {
          m_last_supervised_issued = supervised_iter;
        }
      }

      if (issued == 1)
      {
        m_stats->single_issue_nums[m_id]++;
      }
      else if (issued > 1)
      {
        m_stats->dual_issue_nums[m_id]++;
      }
      else
        abort();  // issued should be > 0

      break;
    }
  }

  // issue stall statistics:
  if (!valid_inst)
    m_stats->shader_cycle_distro[0]++;  // idle or control hazard
  else if (!ready_inst)
    m_stats->shader_cycle_distro[1]++;  // waiting for RAW hazards (possibly due
                                        // to memory)
  else if (!issued_inst)
  {
    m_stats->shader_cycle_distro[2]++;  // pipeline stalled
  }
  return issued_inst;
}

void scheduler_unit::do_on_warp_issued(
    unsigned warp_id, unsigned num_issued,
    const std::vector<shd_warp_t *>::const_iterator &prioritized_iter) {
  m_stats->event_warp_issued(m_shader->get_sid(), warp_id, num_issued,
                             warp(warp_id).get_dynamic_warp_id());
  warp(warp_id).ibuffer_step();
}

void scheduler_unit::replay_do_on_warp_issued(
    unsigned warp_id, unsigned num_issued,
    const std::vector<shd_warp_t *>::const_iterator &prioritized_iter, int MEM_ON) {
  m_stats->event_warp_issued(m_shader->get_sid(), warp_id, num_issued,
                             warp(warp_id).get_dynamic_warp_id());
  if(!MEM_ON)
    warp(warp_id).replay_buffer_step();
  if(MEM_ON)
    warp(warp_id).replay_buffer_step_mem();
}

void scheduler_unit::replay_do_on_warp_issued_mem(
    unsigned warp_id, unsigned num_issued,
    const std::vector<shd_warp_t *>::const_iterator &prioritized_iter, int MEM_ON) {
  m_stats->event_warp_issued(m_shader->get_sid(), warp_id, num_issued,
                             warp(warp_id).get_dynamic_warp_id());
  warp(warp_id).replay_buffer_step_mem();
}

bool scheduler_unit::sort_warps_by_oldest_dynamic_id(shd_warp_t *lhs,
                                                     shd_warp_t *rhs) {
  if (rhs && lhs) {
    if (lhs->done_exit() || lhs->waiting()) {
      return false;
    } else if (rhs->done_exit() || rhs->waiting()) {
      return true;
    } else {
      return lhs->get_dynamic_warp_id() < rhs->get_dynamic_warp_id();
    }
  } else {
    return lhs < rhs;
  }
}

void lrr_scheduler::order_warps(int m_cluster_id) {
  order_lrr(m_next_cycle_prioritized_warps, m_supervised_warps,
            m_last_supervised_issued, m_supervised_warps.size());
}

void lrr_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
}

void fast_scheduler::order_warps(int m_cluster_id) {
  order_fast(m_next_cycle_prioritized_warps, m_supervised_warps,
            m_last_supervised_issued, m_supervised_warps.size(),m_cluster_id);
}

void fast_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
  replay_order_fast(m_next_cycle_prioritized_warps, m_supervised_warps,
            m_last_supervised_issued, m_supervised_warps.size(), m_cluster_id, MEM_ON);
}

void gto_scheduler::order_warps(int m_cluster_id) {
  order_by_priority(m_next_cycle_prioritized_warps, m_supervised_warps,
                    m_last_supervised_issued, m_supervised_warps.size(),
                    ORDERING_GREEDY_THEN_PRIORITY_FUNC,
                    scheduler_unit::sort_warps_by_oldest_dynamic_id);
}

void gto_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
}

void oldest_scheduler::order_warps(int m_cluster_id) {
  order_by_priority(m_next_cycle_prioritized_warps, m_supervised_warps,
                    m_last_supervised_issued, m_supervised_warps.size(),
                    ORDERED_PRIORITY_FUNC_ONLY,
                    scheduler_unit::sort_warps_by_oldest_dynamic_id);
}

void oldest_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
}

void two_level_active_scheduler::do_on_warp_issued(
    unsigned warp_id, unsigned num_issued,
    const std::vector<shd_warp_t *>::const_iterator &prioritized_iter) {
  scheduler_unit::do_on_warp_issued(warp_id, num_issued, prioritized_iter);
  if (SCHEDULER_PRIORITIZATION_LRR == m_inner_level_prioritization) {
    std::vector<shd_warp_t *> new_active;
    order_lrr(new_active, m_next_cycle_prioritized_warps, prioritized_iter,
              m_next_cycle_prioritized_warps.size());
    m_next_cycle_prioritized_warps = new_active;
  } else {
    fprintf(stderr, "Unimplemented m_inner_level_prioritization: %d\n",
            m_inner_level_prioritization);
    abort();
  }
}

void two_level_active_scheduler::order_warps(int m_cluster_id) {
  // Move waiting warps to m_pending_warps
  unsigned num_demoted = 0;
  for (std::vector<shd_warp_t *>::iterator iter =
           m_next_cycle_prioritized_warps.begin();
       iter != m_next_cycle_prioritized_warps.end();) {
    bool waiting = (*iter)->waiting();
    for (int i = 0; i < MAX_INPUT_VALUES; i++) {
      const warp_inst_t *inst = (*iter)->ibuffer_next_inst();
      // Is the instruction waiting on a long operation?
      if (inst && inst->in[i] > 0 &&
          this->m_scoreboard->islongop((*iter)->get_warp_id(), inst->in[i])) {
        waiting = true;
      }
    }

    if (waiting) {
      m_pending_warps.push_back(*iter);
      iter = m_next_cycle_prioritized_warps.erase(iter);
      SCHED_DPRINTF("DEMOTED warp_id=%d, dynamic_warp_id=%d\n",
                    (*iter)->get_warp_id(), (*iter)->get_dynamic_warp_id());
      ++num_demoted;
    } else {
      ++iter;
    }
  }

  // If there is space in m_next_cycle_prioritized_warps, promote the next
  // m_pending_warps
  unsigned num_promoted = 0;
  if (SCHEDULER_PRIORITIZATION_SRR == m_outer_level_prioritization) {
    while (m_next_cycle_prioritized_warps.size() < m_max_active_warps) {
      m_next_cycle_prioritized_warps.push_back(m_pending_warps.front());
      m_pending_warps.pop_front();
      SCHED_DPRINTF(
          "PROMOTED warp_id=%d, dynamic_warp_id=%d\n",
          (m_next_cycle_prioritized_warps.back())->get_warp_id(),
          (m_next_cycle_prioritized_warps.back())->get_dynamic_warp_id());
      ++num_promoted;
    }
  } else {
    fprintf(stderr, "Unimplemented m_outer_level_prioritization: %d\n",
            m_outer_level_prioritization);
    abort();
  }
  assert(num_promoted == num_demoted);
}

void two_level_active_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
}

swl_scheduler::swl_scheduler(shader_core_stats *stats, shader_core_ctx *shader,
                             Scoreboard *scoreboard, simt_stack **simt,
                             std::vector<shd_warp_t *> *warp,
                             register_set *sp_out, register_set *dp_out,
                             register_set *sfu_out, register_set *int_out,
                             register_set *tensor_core_out,
                             std::vector<register_set *> &spec_cores_out,
                             register_set *mem_out, int id, char *config_string)
    : scheduler_unit(stats, shader, scoreboard, simt, warp, sp_out, dp_out,
                     sfu_out, int_out, tensor_core_out, spec_cores_out, mem_out,
                     id) {
  unsigned m_prioritization_readin;
  int ret = sscanf(config_string, "warp_limiting:%d:%d",
                   &m_prioritization_readin, &m_num_warps_to_limit);
  assert(2 == ret);
  m_prioritization = (scheduler_prioritization_type)m_prioritization_readin;
  // Currently only GTO is implemented
  assert(m_prioritization == SCHEDULER_PRIORITIZATION_GTO);
  assert(m_num_warps_to_limit <= shader->get_config()->max_warps_per_shader);
}

void swl_scheduler::order_warps(int m_cluster_id) {
  if (SCHEDULER_PRIORITIZATION_GTO == m_prioritization) {
    order_by_priority(m_next_cycle_prioritized_warps, m_supervised_warps,
                      m_last_supervised_issued,
                      MIN(m_num_warps_to_limit, m_supervised_warps.size()),
                      ORDERING_GREEDY_THEN_PRIORITY_FUNC,
                      scheduler_unit::sort_warps_by_oldest_dynamic_id);
  } else {
    fprintf(stderr, "swl_scheduler m_prioritization = %d\n", m_prioritization);
    abort();
  }
}

void swl_scheduler::replay_order_warps(int m_cluster_id, int MEM_ON) {
}

void shader_core_ctx::read_operands() {}

address_type coalesced_segment(address_type addr,
                               unsigned segment_size_lg2bytes) {
  return (addr >> segment_size_lg2bytes);
}

// Returns numbers of addresses in translated_addrs, each addr points to a 4B
// (32-bit) word
unsigned shader_core_ctx::translate_local_memaddr(
    address_type localaddr, unsigned tid, unsigned num_shader,
    unsigned datasize, new_addr_type *translated_addrs) {
  // During functional execution, each thread sees its own memory space for
  // local memory, but these need to be mapped to a shared address space for
  // timing simulation.  We do that mapping here.

  address_type thread_base = 0;
  unsigned max_concurrent_threads = 0;
  if (m_config->gpgpu_local_mem_map) {
    // Dnew = D*N + T%nTpC + nTpC*C
    // N = nTpC*nCpS*nS (max concurent threads)
    // C = nS*K + S (hw cta number per gpu)
    // K = T/nTpC   (hw cta number per core)
    // D = data index
    // T = thread
    // nTpC = number of threads per CTA
    // nCpS = number of CTA per shader
    //
    // for a given local memory address threads in a CTA map to contiguous
    // addresses, then distribute across memory space by CTAs from successive
    // shader cores first, then by successive CTA in same shader core
    thread_base =
        4 * (kernel_padded_threads_per_cta *
                 (m_sid + num_shader * (tid / kernel_padded_threads_per_cta)) +
             tid % kernel_padded_threads_per_cta);
    max_concurrent_threads =
        kernel_padded_threads_per_cta * kernel_max_cta_per_shader * num_shader;
  } else {
    // legacy mapping that maps the same address in the local memory space of
    // all threads to a single contiguous address region
    thread_base = 4 * (m_config->n_thread_per_shader * m_sid + tid);
    max_concurrent_threads = num_shader * m_config->n_thread_per_shader;
  }
  assert(thread_base < 4 /*word size*/ * max_concurrent_threads);

  // If requested datasize > 4B, split into multiple 4B accesses
  // otherwise do one sub-4 byte memory access
  unsigned num_accesses = 0;

  if (datasize >= 4) {
    // >4B access, split into 4B chunks
    assert(datasize % 4 == 0);  // Must be a multiple of 4B
    num_accesses = datasize / 4;
    assert(num_accesses <= MAX_ACCESSES_PER_INSN_PER_THREAD);  // max 32B
    assert(
        localaddr % 4 ==
        0);  // Address must be 4B aligned - required if accessing 4B per
             // request, otherwise access will overflow into next thread's space
    for (unsigned i = 0; i < num_accesses; i++) {
      address_type local_word = localaddr / 4 + i;
      address_type linear_address = local_word * max_concurrent_threads * 4 +
                                    thread_base + LOCAL_GENERIC_START;
      translated_addrs[i] = linear_address;
    }
  } else {
    // Sub-4B access, do only one access
    assert(datasize > 0);
    num_accesses = 1;
    address_type local_word = localaddr / 4;
    address_type local_word_offset = localaddr % 4;
    assert((localaddr + datasize - 1) / 4 ==
           local_word);  // Make sure access doesn't overflow into next 4B chunk
    address_type linear_address = local_word * max_concurrent_threads * 4 +
                                  local_word_offset + thread_base +
                                  LOCAL_GENERIC_START;
    translated_addrs[0] = linear_address;
  }
  return num_accesses;
}

/////////////////////////////////////////////////////////////////////////////////////////
int shader_core_ctx::test_res_bus(int latency) {
  for (unsigned i = 0; i < num_result_bus; i++) {
    if (!m_result_bus[i]->test(latency)) {
      return i;
    }
  }
  return -1;
}

void shader_core_ctx::execute() {
  for (unsigned i = 0; i < num_result_bus; i++) {
    *(m_result_bus[i]) >>= 1;
  }
  for (unsigned n = 0; n < m_num_function_units; n++) {
    unsigned multiplier = m_fu[n]->clock_multiplier();
    for (unsigned c = 0; c < multiplier; c++) m_fu[n]->cycle();
    m_fu[n]->active_lanes_in_pipeline();
    unsigned issue_port = m_issue_port[n];
    register_set &issue_inst = m_pipeline_reg[issue_port];
    warp_inst_t **ready_reg = issue_inst.get_ready();
    if (issue_inst.has_ready() && m_fu[n]->can_issue(**ready_reg)) {
      bool schedule_wb_now = !m_fu[n]->stallable();
      int resbus = -1;
      if (schedule_wb_now &&
          (resbus = test_res_bus((*ready_reg)->latency)) != -1) {
        assert((*ready_reg)->latency < MAX_ALU_LATENCY);
        m_result_bus[resbus]->set((*ready_reg)->latency);
        m_fu[n]->issue(issue_inst);
      } else if (!schedule_wb_now) {
        m_fu[n]->issue(issue_inst);
      } else {
        // stall issue (cannot reserve result bus)
      }
    }
  }
}

void ldst_unit::print_cache_stats(FILE *fp, unsigned &dl1_accesses,
                                  unsigned &dl1_misses) {
  if (m_L1D) {
    m_L1D->print(fp, dl1_accesses, dl1_misses);
  }
}

void ldst_unit::get_cache_stats(cache_stats &cs) {
  // Adds stats to 'cs' from each cache
  if (m_L1D) cs += m_L1D->get_stats();
  if (m_L1C) cs += m_L1C->get_stats();
  if (m_L1T) cs += m_L1T->get_stats();
}

void ldst_unit::get_L1D_sub_stats(struct cache_sub_stats &css) const {
  if (m_L1D) m_L1D->get_sub_stats(css);
}
void ldst_unit::get_L1C_sub_stats(struct cache_sub_stats &css) const {
  if (m_L1C) m_L1C->get_sub_stats(css);
}
void ldst_unit::get_L1T_sub_stats(struct cache_sub_stats &css) const {
  if (m_L1T) m_L1T->get_sub_stats(css);
}

void shader_core_ctx::warp_inst_complete(const warp_inst_t &inst) {
#if 0
      printf("[warp_inst_complete] uid=%u core=%u warp=%u pc=%#x @ time=%llu \n",
             inst.get_uid(), m_sid, inst.warp_id(), inst.pc,  m_gpu->gpu_tot_sim_cycle +  m_gpu->gpu_sim_cycle);
#endif

  tot_inst_ret++;

  if (inst.op_pipe == SP__OP)
    m_stats->m_num_sp_committed[m_sid]++;
  else if (inst.op_pipe == SFU__OP)
    m_stats->m_num_sfu_committed[m_sid]++;
  else if (inst.op_pipe == MEM__OP)
    m_stats->m_num_mem_committed[m_sid]++;

  if (m_config->gpgpu_clock_gated_lanes == false)
    m_stats->m_num_sim_insn[m_sid] += m_config->warp_size;
  else
    m_stats->m_num_sim_insn[m_sid] += inst.active_count();

  m_stats->m_num_sim_winsn[m_sid]++;
  m_gpu->gpu_sim_insn += inst.active_count();
  //tot_inst_exec += inst.active_count();
  
  inst.completed(m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle);
}

void shader_core_ctx::writeback() {
  unsigned max_committed_thread_instructions =
      m_config->warp_size *
      (m_config->pipe_widths[EX_WB]);  // from the functional units
  m_stats->m_pipeline_duty_cycle[m_sid] =
      ((float)(m_stats->m_num_sim_insn[m_sid] -
               m_stats->m_last_num_sim_insn[m_sid])) /
      max_committed_thread_instructions;

  m_stats->m_last_num_sim_insn[m_sid] = m_stats->m_num_sim_insn[m_sid];
  m_stats->m_last_num_sim_winsn[m_sid] = m_stats->m_num_sim_winsn[m_sid];

  warp_inst_t **preg = m_pipeline_reg[EX_WB].get_ready();
  warp_inst_t *pipe_reg = (preg == NULL) ? NULL : *preg;
  while (preg and !pipe_reg->empty()) {
    /*
     * Right now, the writeback stage drains all waiting instructions
     * assuming there are enough ports in the register file or the
     * conflicts are resolved at issue.
     */
    /*
     * The operand collector writeback can generally generate a stall
     * However, here, the pipelines should be un-stallable. This is
     * guaranteed because this is the first time the writeback function
     * is called after the operand collector's step function, which
     * resets the allocations. There is one case which could result in
     * the writeback function returning false (stall), which is when
     * an instruction tries to modify two registers (GPR and predicate)
     * To handle this case, we ignore the return value (thus allowing
     * no stalling).
     */

    m_operand_collector.writeback(*pipe_reg);
    unsigned warp_id = pipe_reg->warp_id();
    if(!pipe_reg->empty())
    {
      if ((pipe_reg->op == LOAD_OP) || (pipe_reg->op == STORE_OP) ||
        (pipe_reg->op == MEMORY_BARRIER_OP) ||
        (pipe_reg->op == TENSOR_CORE_LOAD_OP) ||
        (pipe_reg->op == TENSOR_CORE_STORE_OP)) 
        {
          mem_inst_finish_time = mem_inst_finish_time + (cycles_passed - pipe_reg->get_cycle_issued_warp());
        }
      else
      {
        comp_inst_finish_time = comp_inst_finish_time + (cycles_passed - pipe_reg->get_cycle_issued_warp());
      }
    }
    m_scoreboard->releaseRegisters(pipe_reg);
    if(pipe_reg->space.get_type() == shared_space)
    {
      shared_cycle_cycle = shared_cycle_cycle + (cycles_passed - pipe_reg->get_cycle_issued());
    }
    if((pipe_reg->space.get_type() == const_space) &&
                       (pipe_reg->space.get_type() == param_space_kernel))
    {
      constant_cycle_cycle = constant_cycle_cycle + (cycles_passed - pipe_reg->get_cycle_issued());
    }
    if (pipe_reg->space.get_type() == tex_space)
    {
      texture_cycle_cycle = texture_cycle_cycle + (cycles_passed - pipe_reg->get_cycle_issued());
    }
    if (((pipe_reg->space.get_type() != global_space) &&
                       (pipe_reg->space.get_type() != local_space) &&
                       (pipe_reg->space.get_type() != param_space_local)))
    {
      memory_cycle_cycle = memory_cycle_cycle + (cycles_passed - pipe_reg->get_cycle_issued());
    }

    m_warp[warp_id]->dec_inst_in_pipeline();
    warp_inst_complete(*pipe_reg);
    m_gpu->gpu_sim_insn_last_update_sid = m_sid;
    m_gpu->gpu_sim_insn_last_update = m_gpu->gpu_sim_cycle;
    m_last_inst_gpu_sim_cycle = m_gpu->gpu_sim_cycle;
    m_last_inst_gpu_tot_sim_cycle = m_gpu->gpu_tot_sim_cycle;
    pipe_reg->clear();
    preg = m_pipeline_reg[EX_WB].get_ready();
    pipe_reg = (preg == NULL) ? NULL : *preg;
  }
}

bool ldst_unit::shared_cycle(warp_inst_t &inst, mem_stage_stall_type &rc_fail,
                             mem_stage_access_type &fail_type) {
  if (inst.space.get_type() != shared_space) return true;

  if (inst.active_count() == 0) return true;

  shared_cycle_count++;

  inst.set_cycle_issued(cycles_passed);

  if (inst.has_dispatch_delay()) {
    m_stats->gpgpu_n_shmem_bank_access[m_sid]++;
  }

  bool stall = inst.dispatch_delay();
  if (stall) {
    fail_type = S_MEM;
    rc_fail = BK_CONF;
  } else
    rc_fail = NO_RC_FAIL;
  return !stall;
}

mem_stage_stall_type ldst_unit::process_cache_access(
    cache_t *cache, new_addr_type address, warp_inst_t &inst,
    std::list<cache_event> &events, mem_fetch *mf,
    enum cache_request_status status) {
  mem_stage_stall_type result = NO_RC_FAIL;
  bool write_sent = was_write_sent(events);
  bool read_sent = was_read_sent(events);
  if (write_sent) {
    unsigned inc_ack = (m_config->m_L1D_config.get_mshr_type() == SECTOR_ASSOC)
                           ? (mf->get_data_size() / SECTOR_SIZE)
                           : 1;

    for (unsigned i = 0; i < inc_ack; ++i)
      m_core->inc_store_req(inst.warp_id());
  }
  if (status == HIT) {
    assert(!read_sent);
    inst.accessq_pop_back();
    if (inst.is_load()) {
      for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++)
        if (inst.out[r] > 0) m_pending_writes[inst.warp_id()][inst.out[r]]--;
    }
    if (!write_sent) delete mf;
  } else if (status == RESERVATION_FAIL) {
    result = BK_CONF;
    assert(!read_sent);
    assert(!write_sent);
    delete mf;
  } else {
    assert(status == MISS || status == HIT_RESERVED);
    // inst.clear_active( access.get_warp_mask() ); // threads in mf writeback
    // when mf returns
    inst.accessq_pop_back();
  }
  if (!inst.accessq_empty() && result == NO_RC_FAIL) result = COAL_STALL;
  return result;
}

mem_stage_stall_type ldst_unit::process_memory_access_queue(cache_t *cache,
                                                            warp_inst_t &inst) {
  mem_stage_stall_type result = NO_RC_FAIL;
  if (inst.accessq_empty()) return result;

  if (!cache->data_port_free()) return DATA_PORT_STALL;

  // const mem_access_t &access = inst.accessq_back();
  mem_fetch *mf = m_mf_allocator->alloc(
      inst, inst.accessq_back(),
      m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle);
  std::list<cache_event> events;
  enum cache_request_status status = cache->access(
      mf->get_addr(), mf,
      m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle,
      events);
  return process_cache_access(cache, mf->get_addr(), inst, events, mf, status);
}

mem_stage_stall_type ldst_unit::process_memory_access_queue_l1cache(
    l1_cache *cache, warp_inst_t &inst) {
  mem_stage_stall_type result = NO_RC_FAIL;
  if (inst.accessq_empty()) return result;

  if (m_config->m_L1D_config.l1_latency > 0) {
    for (int j = 0; j < m_config->m_L1D_config.l1_banks;
         j++) {  // We can handle at max l1_banks reqs per cycle

      if (inst.accessq_empty()) return result;

      mem_fetch *mf =
          m_mf_allocator->alloc(inst, inst.accessq_back(),
                                m_core->get_gpu()->gpu_sim_cycle +
                                    m_core->get_gpu()->gpu_tot_sim_cycle);
      unsigned bank_id = m_config->m_L1D_config.set_bank(mf->get_addr());
      assert(bank_id < m_config->m_L1D_config.l1_banks);

      if ((l1_latency_queue[bank_id][m_config->m_L1D_config.l1_latency - 1]) ==
          NULL) {
        l1_latency_queue[bank_id][m_config->m_L1D_config.l1_latency - 1] = mf;

        if (mf->get_inst().is_store()) {
          unsigned inc_ack =
              (m_config->m_L1D_config.get_mshr_type() == SECTOR_ASSOC)
                  ? (mf->get_data_size() / SECTOR_SIZE)
                  : 1;

          for (unsigned i = 0; i < inc_ack; ++i)
            m_core->inc_store_req(inst.warp_id());
        }

        inst.accessq_pop_back();
      } else {
        result = BK_CONF;
        delete mf;
        break;  // do not try again, just break from the loop and try the next
                // cycle
      }
    }
    if (!inst.accessq_empty() && result != BK_CONF) result = COAL_STALL;

    return result;
  } else {
    mem_fetch *mf =
        m_mf_allocator->alloc(inst, inst.accessq_back(),
                              m_core->get_gpu()->gpu_sim_cycle +
                                  m_core->get_gpu()->gpu_tot_sim_cycle);
    std::list<cache_event> events;
    enum cache_request_status status = cache->access(
        mf->get_addr(), mf,
        m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle,
        events);
    return process_cache_access(cache, mf->get_addr(), inst, events, mf,
                                status);
  }
}

void ldst_unit::L1_latency_queue_cycle() {
  for (int j = 0; j < m_config->m_L1D_config.l1_banks; j++) {
    if ((l1_latency_queue[j][0]) != NULL) {
      mem_fetch *mf_next = l1_latency_queue[j][0];
      std::list<cache_event> events;
      enum cache_request_status status =
          m_L1D->access(mf_next->get_addr(), mf_next,
                        m_core->get_gpu()->gpu_sim_cycle +
                            m_core->get_gpu()->gpu_tot_sim_cycle,
                        events);

      bool write_sent = was_write_sent(events);
      bool read_sent = was_read_sent(events);

      if (status == HIT) {
        assert(!read_sent);
        l1_latency_queue[j][0] = NULL;
        if (mf_next->get_inst().is_load()) {
          for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++)
            if (mf_next->get_inst().out[r] > 0) {
              assert(m_pending_writes[mf_next->get_inst().warp_id()]
                                     [mf_next->get_inst().out[r]] > 0);
              unsigned still_pending =
                  --m_pending_writes[mf_next->get_inst().warp_id()]
                                    [mf_next->get_inst().out[r]];
              if (!still_pending) {
                m_pending_writes[mf_next->get_inst().warp_id()].erase(
                    mf_next->get_inst().out[r]);
                if(!mf_next->get_inst().empty())
                {
                  if ((mf_next->get_inst().op == LOAD_OP) || (mf_next->get_inst().op == STORE_OP) ||
                    (mf_next->get_inst().op == MEMORY_BARRIER_OP) ||
                    (mf_next->get_inst().op == TENSOR_CORE_LOAD_OP) ||
                    (mf_next->get_inst().op == TENSOR_CORE_STORE_OP)) 
                    {
                      mem_inst_finish_time = mem_inst_finish_time + (cycles_passed - mf_next->get_inst().get_cycle_issued_warp());
                    }
                  else
                  {
                    comp_inst_finish_time = comp_inst_finish_time + (cycles_passed - mf_next->get_inst().get_cycle_issued_warp());
                  }
                }
                m_scoreboard->releaseRegister(mf_next->get_inst().warp_id(),
                                              mf_next->get_inst().out[r]);

                if(mf_next->get_inst().space.get_type() == shared_space)
                {
                  shared_cycle_cycle = shared_cycle_cycle + (cycles_passed - mf_next->get_inst().get_cycle_issued());
                }
                if((mf_next->get_inst().space.get_type() == const_space) &&
                       (mf_next->get_inst().space.get_type() == param_space_kernel))
                {
                  constant_cycle_cycle = constant_cycle_cycle + (cycles_passed - mf_next->get_inst().get_cycle_issued());
                }
                if (mf_next->get_inst().space.get_type() == tex_space)
                {
                  texture_cycle_cycle = texture_cycle_cycle + (cycles_passed - mf_next->get_inst().get_cycle_issued());
                }
                if (((mf_next->get_inst().space.get_type() != global_space) &&
                       (mf_next->get_inst().space.get_type() != local_space) &&
                       (mf_next->get_inst().space.get_type() != param_space_local)))
                {
                  memory_cycle_cycle = memory_cycle_cycle + (cycles_passed - mf_next->get_inst().get_cycle_issued());
                }
                m_core->warp_inst_complete(mf_next->get_inst());
              }
            }
        }

        // For write hit in WB policy
        if (mf_next->get_inst().is_store() && !write_sent) {
          unsigned dec_ack =
              (m_config->m_L1D_config.get_mshr_type() == SECTOR_ASSOC)
                  ? (mf_next->get_data_size() / SECTOR_SIZE)
                  : 1;

          mf_next->set_reply();

          for (unsigned i = 0; i < dec_ack; ++i) m_core->store_ack(mf_next);
        }

        if (!write_sent) delete mf_next;

      } else if (status == RESERVATION_FAIL) {
        assert(!read_sent);
        assert(!write_sent);
      } else {
        assert(status == MISS || status == HIT_RESERVED);
        l1_latency_queue[j][0] = NULL;
      }
    }

    for (unsigned stage = 0; stage < m_config->m_L1D_config.l1_latency - 1;
         ++stage)
      if (l1_latency_queue[j][stage] == NULL) {
        l1_latency_queue[j][stage] = l1_latency_queue[j][stage + 1];
        l1_latency_queue[j][stage + 1] = NULL;
      }
  }
}

bool ldst_unit::constant_cycle(warp_inst_t &inst, mem_stage_stall_type &rc_fail,
                               mem_stage_access_type &fail_type) {
  if (inst.empty() || ((inst.space.get_type() != const_space) &&
                       (inst.space.get_type() != param_space_kernel)))
    return true;
  if (inst.active_count() == 0) return true;

  constant_cycle_count++;
  inst.set_cycle_issued(cycles_passed);

  mem_stage_stall_type fail;
  if (m_config->perfect_inst_const_cache) {
    fail = NO_RC_FAIL;
    while (inst.accessq_count() > 0) inst.accessq_pop_back();
    if (inst.is_load()) {
      for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++)
        if (inst.out[r] > 0) m_pending_writes[inst.warp_id()][inst.out[r]]--;
    }
  } else {
    fail = process_memory_access_queue(m_L1C, inst);
  }

  if (fail != NO_RC_FAIL) {
    rc_fail = fail;  // keep other fails if this didn't fail.
    fail_type = C_MEM;
    if (rc_fail == BK_CONF or rc_fail == COAL_STALL) {
      m_stats->gpgpu_n_cmem_portconflict++;  // coal stalls aren't really a bank
                                             // conflict, but this maintains
                                             // previous behavior.
    }
  }
  return inst.accessq_empty();  // done if empty.
}

bool ldst_unit::texture_cycle(warp_inst_t &inst, mem_stage_stall_type &rc_fail,
                              mem_stage_access_type &fail_type) {
  if (inst.empty() || inst.space.get_type() != tex_space) return true;
  if (inst.active_count() == 0) return true;

  texture_cycle_count++;
  texture_issue_cycle = texture_issue_cycle + cycles_passed - inst.get_cycle_issued();
  inst.set_cycle_issued(cycles_passed);
  mem_stage_stall_type fail = process_memory_access_queue(m_L1T, inst);
  if (fail != NO_RC_FAIL) {
    rc_fail = fail;  // keep other fails if this didn't fail.
    fail_type = T_MEM;
  }
  return inst.accessq_empty();  // done if empty.
}

bool ldst_unit::memory_cycle(warp_inst_t &inst,
                             mem_stage_stall_type &stall_reason,
                             mem_stage_access_type &access_type) {

  if (inst.empty() || ((inst.space.get_type() != global_space) &&
                       (inst.space.get_type() != local_space) &&
                       (inst.space.get_type() != param_space_local)))
    return true;

  if (inst.active_count() == 0) return true;
  if (inst.accessq_empty()) return true;

  memory_cycle_count++;
  memory_issue_cycle = memory_issue_cycle + cycles_passed - inst.get_cycle_issued();
  inst.set_cycle_issued(cycles_passed);

  mem_stage_stall_type stall_cond = NO_RC_FAIL;
  const mem_access_t &access = inst.accessq_back();

  bool bypassL1D = false;
  if (CACHE_GLOBAL == inst.cache_op || (m_L1D == NULL)) {
    bypassL1D = true;
  } else if (inst.space.is_global()) {  // global memory access
    // skip L1 cache if the option is enabled
    if (m_core->get_config()->gmem_skip_L1D && (CACHE_L1 != inst.cache_op))
      bypassL1D = true;
  }
  if (bypassL1D) {
    // bypass L1 cache
    unsigned control_size =
        inst.is_store() ? WRITE_PACKET_SIZE : READ_PACKET_SIZE;
    unsigned size = access.get_size() + control_size;
    // printf("Interconnect:Addr: %x, size=%d\n",access.get_addr(),size);
    if (m_icnt->full(size, inst.is_store() || inst.isatomic())) {
      stall_cond = ICNT_RC_FAIL;
    } else {
      mem_fetch *mf =
          m_mf_allocator->alloc(inst, access,
                                m_core->get_gpu()->gpu_sim_cycle +
                                    m_core->get_gpu()->gpu_tot_sim_cycle);
      m_icnt->push(mf);
      if(mf->get_inst().space.get_type() == global_space && print_stall_data)
      {
        cout <<"PUSHING_GLOBAL_MEM_PUTTING_IN_ICNT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid() <<" "<<cycles_passed<<"\n";
      }
      inst.accessq_pop_back();
      // inst.clear_active( access.get_warp_mask() );
      if (inst.is_load()) {
        for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++)
          if (inst.out[r] > 0)
            assert(m_pending_writes[inst.warp_id()][inst.out[r]] > 0);
      } else if (inst.is_store())
        m_core->inc_store_req(inst.warp_id());
    }
  } else {
    assert(CACHE_UNDEFINED != inst.cache_op);
    stall_cond = process_memory_access_queue_l1cache(m_L1D, inst);
  }
  if (!inst.accessq_empty() && stall_cond == NO_RC_FAIL)
    stall_cond = COAL_STALL;
  if (stall_cond != NO_RC_FAIL) {
    stall_reason = stall_cond;
    bool iswrite = inst.is_store();
    if (inst.space.is_local())
      access_type = (iswrite) ? L_MEM_ST : L_MEM_LD;
    else
      access_type = (iswrite) ? G_MEM_ST : G_MEM_LD;
  }
  return inst.accessq_empty();
}

bool ldst_unit::response_buffer_full() const {
  return m_response_fifo.size() >= m_config->ldst_unit_response_queue_size;
}

void ldst_unit::fill(mem_fetch *mf) {
  mf->set_status(
      IN_SHADER_LDST_RESPONSE_FIFO,
      m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle);
  m_response_fifo.push_back(mf);
}

void ldst_unit::flush() {
  // Flush L1D cache
  m_L1D->flush();
}

void ldst_unit::invalidate() {
  // Flush L1D cache
  m_L1D->invalidate();
}

simd_function_unit::simd_function_unit(const shader_core_config *config) {
  m_config = config;
  m_dispatch_reg = new warp_inst_t(config);
}

sfu::sfu(register_set *result_port, const shader_core_config *config,
         shader_core_ctx *core)
    : pipelined_simd_unit(result_port, config, config->max_sfu_latency, core) {
  m_name = "SFU";
}

tensor_core::tensor_core(register_set *result_port,
                         const shader_core_config *config,
                         shader_core_ctx *core)
    : pipelined_simd_unit(result_port, config, config->max_tensor_core_latency,
                          core) {
  m_name = "TENSOR_CORE";
}

void sfu::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));

  (*ready_reg)->op_pipe = SFU__OP;
  m_core->incsfu_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

void tensor_core::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));

  (*ready_reg)->op_pipe = TENSOR_CORE__OP;
  m_core->incsfu_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

unsigned pipelined_simd_unit::get_active_lanes_in_pipeline() {
  active_mask_t active_lanes;
  active_lanes.reset();
  if (m_core->get_gpu()->get_config().g_power_simulation_enabled) {
    for (unsigned stage = 0; (stage + 1) < m_pipeline_depth; stage++) {
      if (!m_pipeline_reg[stage]->empty())
        active_lanes |= m_pipeline_reg[stage]->get_active_mask();
    }
  }
  return active_lanes.count();
}

void ldst_unit::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incfumemactivelanes_stat(active_count);
}

void sp_unit::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incspactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}
void dp_unit::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incspactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}
void specialized_unit::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incspactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}

void int_unit::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incspactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}
void sfu::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incsfuactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}

void tensor_core::active_lanes_in_pipeline() {
  unsigned active_count = pipelined_simd_unit::get_active_lanes_in_pipeline();
  assert(active_count <= m_core->get_config()->warp_size);
  m_core->incsfuactivelanes_stat(active_count);
  m_core->incfuactivelanes_stat(active_count);
  m_core->incfumemactivelanes_stat(active_count);
}

sp_unit::sp_unit(register_set *result_port, const shader_core_config *config,
                 shader_core_ctx *core)
    : pipelined_simd_unit(result_port, config, config->max_sp_latency, core) {
  m_name = "SP ";
}

specialized_unit::specialized_unit(register_set *result_port,
                                   const shader_core_config *config,
                                   shader_core_ctx *core, unsigned supported_op,
                                   char *unit_name, unsigned latency)
    : pipelined_simd_unit(result_port, config, latency, core) {
  m_name = unit_name;
  m_supported_op = supported_op;
}

dp_unit::dp_unit(register_set *result_port, const shader_core_config *config,
                 shader_core_ctx *core)
    : pipelined_simd_unit(result_port, config, config->max_dp_latency, core) {
  m_name = "DP ";
}

int_unit::int_unit(register_set *result_port, const shader_core_config *config,
                   shader_core_ctx *core)
    : pipelined_simd_unit(result_port, config, config->max_int_latency, core) {
  m_name = "INT ";
}

void sp_unit ::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));
  (*ready_reg)->op_pipe = SP__OP;
  m_core->incsp_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

void dp_unit ::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));
  (*ready_reg)->op_pipe = DP__OP;
  m_core->incsp_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

void specialized_unit ::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));
  (*ready_reg)->op_pipe = SPECIALIZED__OP;
  m_core->incsp_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

void int_unit ::issue(register_set &source_reg) {
  warp_inst_t **ready_reg = source_reg.get_ready();
  // m_core->incexecstat((*ready_reg));
  (*ready_reg)->op_pipe = INTP__OP;
  m_core->incsp_stat(m_core->get_config()->warp_size, (*ready_reg)->latency);
  pipelined_simd_unit::issue(source_reg);
}

pipelined_simd_unit::pipelined_simd_unit(register_set *result_port,
                                         const shader_core_config *config,
                                         unsigned max_latency,
                                         shader_core_ctx *core)
    : simd_function_unit(config) {
  m_result_port = result_port;
  m_pipeline_depth = max_latency;
  m_pipeline_reg = new warp_inst_t *[m_pipeline_depth];
  for (unsigned i = 0; i < m_pipeline_depth; i++)
    m_pipeline_reg[i] = new warp_inst_t(config);
  m_core = core;
  active_insts_in_pipeline = 0;
}

void pipelined_simd_unit::cycle() {
  if (!m_pipeline_reg[0]->empty()) {
    m_result_port->move_in(m_pipeline_reg[0]);
    assert(active_insts_in_pipeline > 0);
    active_insts_in_pipeline--;
  }
  if (active_insts_in_pipeline) {
    for (unsigned stage = 0; (stage + 1) < m_pipeline_depth; stage++)
      move_warp(m_pipeline_reg[stage], m_pipeline_reg[stage + 1]);
  }
  if (!m_dispatch_reg->empty()) {
    if (!m_dispatch_reg->dispatch_delay()) {
      int start_stage =
          m_dispatch_reg->latency - m_dispatch_reg->initiation_interval;
      move_warp(m_pipeline_reg[start_stage], m_dispatch_reg);
      active_insts_in_pipeline++;
    }
  }
  occupied >>= 1;
}

void pipelined_simd_unit::issue(register_set &source_reg) {
  // move_warp(m_dispatch_reg,source_reg);
  warp_inst_t **ready_reg = source_reg.get_ready();
  m_core->incexecstat((*ready_reg));
  // source_reg.move_out_to(m_dispatch_reg);
  simd_function_unit::issue(source_reg);
}

/*
    virtual void issue( register_set& source_reg )
    {
        //move_warp(m_dispatch_reg,source_reg);
        //source_reg.move_out_to(m_dispatch_reg);
        simd_function_unit::issue(source_reg);
    }
*/

void ldst_unit::init(mem_fetch_interface *icnt,
                     shader_core_mem_fetch_allocator *mf_allocator,
                     shader_core_ctx *core, opndcoll_rfu_t *operand_collector,
                     Scoreboard *scoreboard, const shader_core_config *config,
                     const memory_config *mem_config, shader_core_stats *stats,
                     unsigned sid, unsigned tpc) {
  m_memory_config = mem_config;
  m_icnt = icnt;
  m_mf_allocator = mf_allocator;
  m_core = core;
  m_operand_collector = operand_collector;
  m_scoreboard = scoreboard;
  m_stats = stats;
  m_sid = sid;
  m_tpc = tpc;
#define STRSIZE 1024
  char L1T_name[STRSIZE];
  char L1C_name[STRSIZE];
  snprintf(L1T_name, STRSIZE, "L1T_%03d", m_sid);
  snprintf(L1C_name, STRSIZE, "L1C_%03d", m_sid);
  m_L1T = new tex_cache(L1T_name, m_config->m_L1T_config, m_sid,
                        get_shader_texture_cache_id(), icnt, IN_L1T_MISS_QUEUE,
                        IN_SHADER_L1T_ROB);
  m_L1C = new read_only_cache(L1C_name, m_config->m_L1C_config, m_sid,
                              get_shader_constant_cache_id(), icnt,
                              IN_L1C_MISS_QUEUE);
  m_L1D = NULL;
  m_mem_rc = NO_RC_FAIL;
  m_num_writeback_clients =
      5;  // = shared memory, global/local (uncached), L1D, L1T, L1C
  m_writeback_arb = 0;
  m_next_global = NULL;
  m_last_inst_gpu_sim_cycle = 0;
  m_last_inst_gpu_tot_sim_cycle = 0;
}

ldst_unit::ldst_unit(mem_fetch_interface *icnt,
                     shader_core_mem_fetch_allocator *mf_allocator,
                     shader_core_ctx *core, opndcoll_rfu_t *operand_collector,
                     Scoreboard *scoreboard, const shader_core_config *config,
                     const memory_config *mem_config, shader_core_stats *stats,
                     unsigned sid, unsigned tpc)
    : pipelined_simd_unit(NULL, config, config->smem_latency, core),
      m_next_wb(config) {
  assert(config->smem_latency > 1);
  init(icnt, mf_allocator, core, operand_collector, scoreboard, config,
       mem_config, stats, sid, tpc);
  if (!m_config->m_L1D_config.disabled()) {
    char L1D_name[STRSIZE];
    snprintf(L1D_name, STRSIZE, "L1D_%03d", m_sid);
    m_L1D = new l1_cache(L1D_name, m_config->m_L1D_config, m_sid,
                         get_shader_normal_cache_id(), m_icnt, m_mf_allocator,
                         IN_L1D_MISS_QUEUE, core->get_gpu());

    l1_latency_queue.resize(m_config->m_L1D_config.l1_banks);
    assert(m_config->m_L1D_config.l1_latency > 0);

    for (unsigned j = 0; j < m_config->m_L1D_config.l1_banks; j++)
      l1_latency_queue[j].resize(m_config->m_L1D_config.l1_latency,
                                 (mem_fetch *)NULL);
  }
  m_name = "MEM ";
}

ldst_unit::ldst_unit(mem_fetch_interface *icnt,
                     shader_core_mem_fetch_allocator *mf_allocator,
                     shader_core_ctx *core, opndcoll_rfu_t *operand_collector,
                     Scoreboard *scoreboard, const shader_core_config *config,
                     const memory_config *mem_config, shader_core_stats *stats,
                     unsigned sid, unsigned tpc, l1_cache *new_l1d_cache)
    : pipelined_simd_unit(NULL, config, 3, core),
      m_L1D(new_l1d_cache),
      m_next_wb(config) {
  init(icnt, mf_allocator, core, operand_collector, scoreboard, config,
       mem_config, stats, sid, tpc);
}

void ldst_unit::issue(register_set &reg_set) {
  warp_inst_t *inst = *(reg_set.get_ready());

  // record how many pending register writes/memory accesses there are for this
  // instruction
  assert(inst->empty() == false);
  if (inst->is_load() and inst->space.get_type() != shared_space) {
    unsigned warp_id = inst->warp_id();
    unsigned n_accesses = inst->accessq_count();
    for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
      unsigned reg_id = inst->out[r];
      if (reg_id > 0) {
        m_pending_writes[warp_id][reg_id] += n_accesses;
      }
    }
  }

  inst->op_pipe = MEM__OP;
  // stat collection
  m_core->mem_instruction_stats(*inst);
  m_core->incmem_stat(m_core->get_config()->warp_size, 1);
  pipelined_simd_unit::issue(reg_set);
}

void ldst_unit::writeback() {
  // process next instruction that is going to writeback
  if (!m_next_wb.empty()) {
    if (m_operand_collector->writeback(m_next_wb)) {
      //CODE STARTED BY ISHITA
      int counter_reg = 0;
      if(m_next_wb.space.get_type() == global_space && m_next_wb.is_load())
      {
        for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
          if (m_next_wb.out[r] > 0) {
            counter_reg = m_pending_writes[m_next_wb.warp_id()][m_next_wb.out[r]];
          }
        }
        if(print_stall_data)
          cout <<"PUSHING_GLOBAL_MEM_COMP_FINISH "<<m_next_wb.pc<<" "<<m_next_wb.warp_id()<<" "<<m_next_wb.get_sid()<<" "<<cycles_passed<<" "<<counter_reg<<"\n";
      }
      //CODE ENDED BY ISHITA
      bool insn_completed = false;
      for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
        if (m_next_wb.out[r] > 0) {
          if (m_next_wb.space.get_type() != shared_space) {
            assert(m_pending_writes[m_next_wb.warp_id()][m_next_wb.out[r]] > 0);
            unsigned still_pending =
                --m_pending_writes[m_next_wb.warp_id()][m_next_wb.out[r]];
            if (!still_pending) {
              m_pending_writes[m_next_wb.warp_id()].erase(m_next_wb.out[r]);
              if(!m_next_wb.empty())
              {
                if ((m_next_wb.op == LOAD_OP) || (m_next_wb.op == STORE_OP) ||
                  (m_next_wb.op == MEMORY_BARRIER_OP) ||
                  (m_next_wb.op == TENSOR_CORE_LOAD_OP) ||
                  (m_next_wb.op == TENSOR_CORE_STORE_OP)) 
                  {
                    mem_inst_finish_time = mem_inst_finish_time + (cycles_passed - m_next_wb.get_cycle_issued_warp());
                  }
                else
                {
                  comp_inst_finish_time = comp_inst_finish_time + (cycles_passed - m_next_wb.get_cycle_issued_warp());
                }
              }
              m_scoreboard->releaseRegister(m_next_wb.warp_id(),
                                            m_next_wb.out[r]);
              if(m_next_wb.space.get_type() == shared_space)
              {
                shared_cycle_cycle = shared_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
              }
              insn_completed = true;

              if((m_next_wb.space.get_type() == const_space) &&
                       (m_next_wb.space.get_type() == param_space_kernel))
              {
                constant_cycle_cycle = constant_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
              }

              if (m_next_wb.space.get_type() == tex_space)
              {
                texture_cycle_cycle = texture_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
              }

              if (((m_next_wb.space.get_type() != global_space) &&
                       (m_next_wb.space.get_type() != local_space) &&
                       (m_next_wb.space.get_type() != param_space_local)))
              {
                memory_cycle_cycle = memory_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
              }
            }
          } else {  // shared
            if(!m_next_wb.empty())
            {
              if ((m_next_wb.op == LOAD_OP) || (m_next_wb.op == STORE_OP) ||
                (m_next_wb.op == MEMORY_BARRIER_OP) ||
                (m_next_wb.op == TENSOR_CORE_LOAD_OP) ||
                (m_next_wb.op == TENSOR_CORE_STORE_OP)) 
                {
                  mem_inst_finish_time = mem_inst_finish_time + (cycles_passed - m_next_wb.get_cycle_issued_warp());
                }
              else
              {
                comp_inst_finish_time = comp_inst_finish_time + (cycles_passed - m_next_wb.get_cycle_issued_warp());
              }
            }
            m_scoreboard->releaseRegister(m_next_wb.warp_id(),
                                          m_next_wb.out[r]);
            if(m_next_wb.space.get_type() == shared_space)
            {
              shared_cycle_cycle = shared_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
            }
            if((m_next_wb.space.get_type() == const_space) &&
                       (m_next_wb.space.get_type() == param_space_kernel))
            {
              constant_cycle_cycle = constant_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
            }

            if (m_next_wb.space.get_type() == tex_space)
            {
              texture_cycle_cycle = texture_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
            }

            if (((m_next_wb.space.get_type() != global_space) &&
                       (m_next_wb.space.get_type() != local_space) &&
                       (m_next_wb.space.get_type() != param_space_local)))
            {
              memory_cycle_cycle = memory_cycle_cycle + (cycles_passed - m_next_wb.get_cycle_issued());
            }
            insn_completed = true;
          }
        }
      }
      if (insn_completed) {
        m_core->warp_inst_complete(m_next_wb);
      }
      m_next_wb.clear();
      m_last_inst_gpu_sim_cycle = m_core->get_gpu()->gpu_sim_cycle;
      m_last_inst_gpu_tot_sim_cycle = m_core->get_gpu()->gpu_tot_sim_cycle;
    }
  }

  unsigned serviced_client = -1;
  for (unsigned c = 0; m_next_wb.empty() && (c < m_num_writeback_clients);
       c++) {
    unsigned next_client = (c + m_writeback_arb) % m_num_writeback_clients;
    switch (next_client) {
      case 0:  // shared memory
        if (!m_pipeline_reg[0]->empty()) {
          m_next_wb = *m_pipeline_reg[0];
          if (m_next_wb.isatomic()) {
            m_next_wb.do_atomic();
            m_core->decrement_atomic_count(m_next_wb.warp_id(),
                                           m_next_wb.active_count());
          }
          m_core->dec_inst_in_pipeline(m_pipeline_reg[0]->warp_id());
          m_pipeline_reg[0]->clear();
          serviced_client = next_client;
        }
        break;
      case 1:  // texture response
        if (m_L1T->access_ready()) {
          mem_fetch *mf = m_L1T->next_access();
          m_next_wb = mf->get_inst();
          delete mf;
          serviced_client = next_client;
          if(mf && !mf->get_inst().empty())
          {
            mf->get_inst().set_cycle_issued(cycles_passed);
          }
          if(!m_next_wb.empty())
          {
            reach_L1_from_tex = reach_L1_from_tex + cycles_passed - mf->get_cycle_issued();
            m_next_wb.set_cycle_issued(cycles_passed);
          }
        }
        break;
      case 2:  // const cache response
        if (m_L1C->access_ready()) {
          mem_fetch *mf = m_L1C->next_access();
          m_next_wb = mf->get_inst();
          delete mf;
          serviced_client = next_client;
        }
        break;
      case 3:  // global/local
        if (m_next_global) {
          m_next_wb = m_next_global->get_inst();
          if(!m_next_wb.empty())
          {
            //reached_global_from_glob = reached_global_from_glob + cycles_passed - m_next_wb.get_cycle_issued();
            reached_global_from_glob = reached_global_from_glob + cycles_passed - m_next_global->get_cycle_issued();
            if(!m_next_wb.empty())
              m_next_wb.set_cycle_issued(cycles_passed);
            m_next_wb.set_cycle_issued(cycles_passed);
          }
          if(print_stall_data)
            cout <<"PUSHING_GLOBAL_MEM_FOUND_HERE "<<m_next_global->get_inst().pc<<" "<<m_next_global->get_inst().warp_id()<<" "<<m_next_global->get_inst().get_sid()<<" "<<cycles_passed<<"\n";
          simple_dram_count = simple_dram_count + 1; 
          if (m_next_global->isatomic()) {
            m_core->decrement_atomic_count(
                m_next_global->get_wid(),
                m_next_global->get_access_warp_mask().count());
          }
          delete m_next_global;
          m_next_global = NULL;
          serviced_client = next_client;
        }
        break;
      case 4:
        if (m_L1D && m_L1D->access_ready()) {
          mem_fetch *mf = m_L1D->next_access();
          m_next_wb = mf->get_inst();
          delete mf;
          serviced_client = next_client;
        }
        break;
      default:
        abort();
    }
  }
  // update arbitration priority only if:
  // 1. the writeback buffer was available
  // 2. a client was serviced
  if (serviced_client != (unsigned)-1) {
    m_writeback_arb = (serviced_client + 1) % m_num_writeback_clients;
  }
}

unsigned ldst_unit::clock_multiplier() const {
  // to model multiple read port, we give multiple cycles for the memory units
  if (m_config->mem_unit_ports)
    return m_config->mem_unit_ports;
  else
    return m_config->mem_warp_parts;
}
/*
void ldst_unit::issue( register_set &reg_set )
{
        warp_inst_t* inst = *(reg_set.get_ready());
   // stat collection
   m_core->mem_instruction_stats(*inst);

   // record how many pending register writes/memory accesses there are for this
instruction assert(inst->empty() == false); if (inst->is_load() and
inst->space.get_type() != shared_space) { unsigned warp_id = inst->warp_id();
      unsigned n_accesses = inst->accessq_count();
      for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
         unsigned reg_id = inst->out[r];
         if (reg_id > 0) {
            m_pending_writes[warp_id][reg_id] += n_accesses;
         }
      }
   }

   pipelined_simd_unit::issue(reg_set);
}
*/
void ldst_unit::cycle() {
  writeback();
  for (int i = 0; i < m_config->reg_file_port_throughput; ++i)
    m_operand_collector->step();
  for (unsigned stage = 0; (stage + 1) < m_pipeline_depth; stage++)
    if (m_pipeline_reg[stage]->empty() && !m_pipeline_reg[stage + 1]->empty())
      move_warp(m_pipeline_reg[stage], m_pipeline_reg[stage + 1]);

  if (!m_response_fifo.empty()) {
    mem_fetch *mf = m_response_fifo.front();
    if (mf->get_access_type() == TEXTURE_ACC_R) {
      if (m_L1T->fill_port_free()) {
        m_L1T->fill(mf, m_core->get_gpu()->gpu_sim_cycle +
                            m_core->get_gpu()->gpu_tot_sim_cycle);
        m_response_fifo.pop_front();
        if(mf && !mf->get_inst().empty())
        {
          reach_tex_from_l2 = reach_tex_from_l2 + cycles_passed - mf->get_cycle_issued();
          mf->set_cycle_issued(cycles_passed);
        }
        if(mf && !mf->get_inst().empty())
        {
          CLUSTER_TO_SHADER_QUEUE_count = CLUSTER_TO_SHADER_QUEUE_count + 1;
          CLUSTER_TO_SHADER_QUEUE_cycle = CLUSTER_TO_SHADER_QUEUE_cycle + m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle(); 
          if(print_stall_data)
            cout <<"CLUSTER_SHADER_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<"\n";
        }
      }
    } else if (mf->get_access_type() == CONST_ACC_R) {
      if (m_L1C->fill_port_free()) {
        mf->set_status(IN_SHADER_FETCHED,
                       m_core->get_gpu()->gpu_sim_cycle +
                           m_core->get_gpu()->gpu_tot_sim_cycle);
        m_L1C->fill(mf, m_core->get_gpu()->gpu_sim_cycle +
                            m_core->get_gpu()->gpu_tot_sim_cycle);
        m_response_fifo.pop_front();
        if(mf && !mf->get_inst().empty())
        {
          CLUSTER_TO_SHADER_QUEUE_count = CLUSTER_TO_SHADER_QUEUE_count + 1;
          CLUSTER_TO_SHADER_QUEUE_cycle = CLUSTER_TO_SHADER_QUEUE_cycle + m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle(); 
          if(print_stall_data)
            cout <<"CLUSTER_SHADER_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<"\n";
        }
      }
    } else {
      if (mf->get_type() == WRITE_ACK ||
          (m_config->gpgpu_perfect_mem && mf->get_is_write())) {
        m_core->store_ack(mf);
        m_response_fifo.pop_front();
        if(mf && !mf->get_inst().empty())
        {
          CLUSTER_TO_SHADER_QUEUE_count = CLUSTER_TO_SHADER_QUEUE_count + 1;
          CLUSTER_TO_SHADER_QUEUE_cycle = CLUSTER_TO_SHADER_QUEUE_cycle + m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle(); 
          if(print_stall_data)
            cout <<"CLUSTER_SHADER_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<"\n";
        }
        delete mf;
      } else {
        assert(!mf->get_is_write());  // L1 cache is write evict, allocate line
                                      // on load miss only

        bool bypassL1D = false;
        if (CACHE_GLOBAL == mf->get_inst().cache_op || (m_L1D == NULL)) {
          bypassL1D = true;
        } else if (mf->get_access_type() == GLOBAL_ACC_R ||
                   mf->get_access_type() ==
                       GLOBAL_ACC_W) {  // global memory access
          if (m_core->get_config()->gmem_skip_L1D) bypassL1D = true;
        }
        if (bypassL1D) {
          if (m_next_global == NULL) {
            mf->set_status(IN_SHADER_FETCHED,
                           m_core->get_gpu()->gpu_sim_cycle +
                               m_core->get_gpu()->gpu_tot_sim_cycle);
            m_response_fifo.pop_front();
            if(mf && !mf->get_inst().empty())
            {
              reach_glob_from_icnt = reach_glob_from_icnt + cycles_passed - mf->get_cycle_issued();
              mf->set_cycle_issued(cycles_passed);
            }
            if(print_stall_data)
              cout <<"PUSHING_GLOBAL_MEM_PUSHED_HERE "<<m_next_global->get_inst().pc<<" "<<m_next_global->get_inst().warp_id()<<" "<<m_next_global->get_inst().get_sid()<<" "<<cycles_passed<<"\n";
            m_next_global = mf;
            if(mf && !mf->get_inst().empty())
            {
              CLUSTER_TO_SHADER_QUEUE_count = CLUSTER_TO_SHADER_QUEUE_count + 1;
              CLUSTER_TO_SHADER_QUEUE_cycle = CLUSTER_TO_SHADER_QUEUE_cycle + m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle(); 
              if(print_stall_data)
                cout <<"CLUSTER_SHADER_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<"\n";
            }
          }
        } else {
          if (m_L1D->fill_port_free()) {
            m_L1D->fill(mf, m_core->get_gpu()->gpu_sim_cycle +
                                m_core->get_gpu()->gpu_tot_sim_cycle);
            m_response_fifo.pop_front();
            if(mf && !mf->get_inst().empty())
            {
              CLUSTER_TO_SHADER_QUEUE_count = CLUSTER_TO_SHADER_QUEUE_count + 1;
              CLUSTER_TO_SHADER_QUEUE_cycle = CLUSTER_TO_SHADER_QUEUE_cycle + m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle(); 
              if(print_stall_data)
                cout <<"CLUSTER_SHADER_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_core->get_gpu()->gpu_sim_cycle + m_core->get_gpu()->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<"\n";
            }
          }
        }
      }
    }
  }

  m_L1T->cycle();
  m_L1C->cycle();
  if (m_L1D) {
    m_L1D->cycle();
    if (m_config->m_L1D_config.l1_latency > 0) L1_latency_queue_cycle();
  }

  warp_inst_t &pipe_reg = *m_dispatch_reg;
  enum mem_stage_stall_type rc_fail = NO_RC_FAIL;
  mem_stage_access_type type;
  bool done = true;
  done &= shared_cycle(pipe_reg, rc_fail, type);
  done &= constant_cycle(pipe_reg, rc_fail, type);
  done &= texture_cycle(pipe_reg, rc_fail, type);
  done &= memory_cycle(pipe_reg, rc_fail, type);
  m_mem_rc = rc_fail;

  if (!done) {  // log stall types and return
    assert(rc_fail != NO_RC_FAIL);
    m_stats->gpgpu_n_stall_shd_mem++;
    m_stats->gpu_stall_shd_mem_breakdown[type][rc_fail]++;
    return;
  }

  if (!pipe_reg.empty()) {
    unsigned warp_id = pipe_reg.warp_id();
    if (pipe_reg.is_load()) {
      if (pipe_reg.space.get_type() == shared_space) {
        if (m_pipeline_reg[m_config->smem_latency - 1]->empty()) {
          // new shared memory request
          move_warp(m_pipeline_reg[m_config->smem_latency - 1], m_dispatch_reg);
          m_dispatch_reg->clear();
        }
      } else {
        // if( pipe_reg.active_count() > 0 ) {
        //    if( !m_operand_collector->writeback(pipe_reg) )
        //        return;
        //}

        bool pending_requests = false;
        for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
          unsigned reg_id = pipe_reg.out[r];
          if (reg_id > 0) {
            if (m_pending_writes[warp_id].find(reg_id) !=
                m_pending_writes[warp_id].end()) {
              if (m_pending_writes[warp_id][reg_id] > 0) {
                pending_requests = true;
                break;
              } else {
                // this instruction is done already
                m_pending_writes[warp_id].erase(reg_id);
              }
            }
          }
        }
        if (!pending_requests) {
          m_core->warp_inst_complete(*m_dispatch_reg);
          if(!pipe_reg.empty())
          {
            if ((pipe_reg.op == LOAD_OP) || (pipe_reg.op == STORE_OP) ||
              (pipe_reg.op == MEMORY_BARRIER_OP) ||
              (pipe_reg.op == TENSOR_CORE_LOAD_OP) ||
              (pipe_reg.op == TENSOR_CORE_STORE_OP)) 
              {
                mem_inst_finish_time = mem_inst_finish_time + (cycles_passed - pipe_reg.get_cycle_issued_warp());
              }
            else
            {
              comp_inst_finish_time = comp_inst_finish_time + (cycles_passed - pipe_reg.get_cycle_issued_warp());
            }
          }
          m_scoreboard->releaseRegisters(m_dispatch_reg);
          if(pipe_reg.space.get_type() == shared_space)
          {
            shared_cycle_cycle = shared_cycle_cycle + (cycles_passed - pipe_reg.get_cycle_issued());
          }
          if((pipe_reg.space.get_type() == const_space) &&
                       (pipe_reg.space.get_type() == param_space_kernel))
          {
            constant_cycle_cycle = constant_cycle_cycle + (cycles_passed - pipe_reg.get_cycle_issued());
          }
          if (pipe_reg.space.get_type() == tex_space)
          {
            texture_cycle_cycle = texture_cycle_cycle + (cycles_passed - pipe_reg.get_cycle_issued());
          }
          if (((pipe_reg.space.get_type() != global_space) &&
                       (pipe_reg.space.get_type() != local_space) &&
                       (pipe_reg.space.get_type() != param_space_local)))
          {
            memory_cycle_cycle = memory_cycle_cycle + (cycles_passed - pipe_reg.get_cycle_issued());
          }
        }
        m_core->dec_inst_in_pipeline(warp_id);
        m_dispatch_reg->clear();
      }
    } else {
      // stores exit pipeline here
      m_core->dec_inst_in_pipeline(warp_id);
      m_core->warp_inst_complete(*m_dispatch_reg);
      m_dispatch_reg->clear();
    }
  }
}

void shader_core_ctx::register_cta_thread_exit(unsigned cta_num,
                                               kernel_info_t *kernel) {
  assert(m_cta_status[cta_num] > 0);
  m_cta_status[cta_num]--;
  if (!m_cta_status[cta_num]) {
    // Increment the completed CTAs
    m_stats->ctas_completed++;
    m_gpu->inc_completed_cta();
    m_n_active_cta--;
    m_barriers.deallocate_barrier(cta_num);
    shader_CTA_count_unlog(m_sid, 1);

    SHADER_DPRINTF(
        LIVENESS,
        "GPGPU-Sim uArch: Finished CTA #%u (%lld,%lld), %u CTAs running\n",
        cta_num, m_gpu->gpu_sim_cycle, m_gpu->gpu_tot_sim_cycle,
        m_n_active_cta);

    if (m_n_active_cta == 0) {
      SHADER_DPRINTF(
          LIVENESS,
          "GPGPU-Sim uArch: Empty (last released kernel %u \'%s\').\n",
          kernel->get_uid(), kernel->name().c_str());
      fflush(stdout);

      // Shader can only be empty when no more cta are dispatched
      if (kernel != m_kernel) {
        assert(m_kernel == NULL || !m_gpu->kernel_more_cta_left(m_kernel));
      }
      m_kernel = NULL;
    }

    // Jin: for concurrent kernels on sm
    release_shader_resource_1block(cta_num, *kernel);
    kernel->dec_running();
    if (!m_gpu->kernel_more_cta_left(kernel)) {
      if (!kernel->running()) {
        SHADER_DPRINTF(LIVENESS,
                       "GPGPU-Sim uArch: GPU detected kernel %u \'%s\' "
                       "finished on shader %u.\n",
                       kernel->get_uid(), kernel->name().c_str(), m_sid);

        if (m_kernel == kernel) m_kernel = NULL;
        m_gpu->set_kernel_done(kernel);
      }
    }
  }
}

void gpgpu_sim::shader_print_runtime_stat(FILE *fout) {
  /*
 fprintf(fout, "SHD_INSN: ");
 for (unsigned i=0;i<m_n_shader;i++)
    fprintf(fout, "%u ",m_sc[i]->get_num_sim_insn());
 fprintf(fout, "\n");
 fprintf(fout, "SHD_THDS: ");
 for (unsigned i=0;i<m_n_shader;i++)
    fprintf(fout, "%u ",m_sc[i]->get_not_completed());
 fprintf(fout, "\n");
 fprintf(fout, "SHD_DIVG: ");
 for (unsigned i=0;i<m_n_shader;i++)
    fprintf(fout, "%u ",m_sc[i]->get_n_diverge());
 fprintf(fout, "\n");

 fprintf(fout, "THD_INSN: ");
 for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++)
    fprintf(fout, "%d ", m_sc[0]->get_thread_n_insn(i) );
 fprintf(fout, "\n");
 */
}

void gpgpu_sim::shader_print_scheduler_stat(FILE *fout,
                                            bool print_dynamic_info) const {
  fprintf(fout, "ctas_completed %d, ", m_shader_stats->ctas_completed);
  // Print out the stats from the sampling shader core
  const unsigned scheduler_sampling_core =
      m_shader_config->gpgpu_warp_issue_shader;
#define STR_SIZE 55
  char name_buff[STR_SIZE];
  name_buff[STR_SIZE - 1] = '\0';
  const std::vector<unsigned> &distro =
      print_dynamic_info
          ? m_shader_stats->get_dynamic_warp_issue()[scheduler_sampling_core]
          : m_shader_stats->get_warp_slot_issue()[scheduler_sampling_core];
  if (print_dynamic_info) {
    snprintf(name_buff, STR_SIZE - 1, "dynamic_warp_id");
  } else {
    snprintf(name_buff, STR_SIZE - 1, "warp_id");
  }
  fprintf(fout, "Shader %d %s issue ditsribution:\n", scheduler_sampling_core,
          name_buff);
  const unsigned num_warp_ids = distro.size();
  // First print out the warp ids
  fprintf(fout, "%s:\n", name_buff);
  for (unsigned warp_id = 0; warp_id < num_warp_ids; ++warp_id) {
    fprintf(fout, "%d, ", warp_id);
  }

  fprintf(fout, "\ndistro:\n");
  // Then print out the distribution of instuctions issued
  for (std::vector<unsigned>::const_iterator iter = distro.begin();
       iter != distro.end(); iter++) {
    fprintf(fout, "%d, ", *iter);
  }
  fprintf(fout, "\n");
}

void gpgpu_sim::shader_print_cache_stats(FILE *fout) const {
  // L1I
  struct cache_sub_stats total_css;
  struct cache_sub_stats css;

  if (!m_shader_config->m_L1I_config.disabled()) {
    total_css.clear();
    css.clear();
    fprintf(fout, "\n========= Core cache stats =========\n");
    fprintf(fout, "L1I_cache:\n");
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; ++i) {
      m_cluster[i]->get_L1I_sub_stats(css);
      total_css += css;
    }
    fprintf(fout, "\tL1I_total_cache_accesses = %llu\n", total_css.accesses);
    L1I_total_cache_accesses += total_css.accesses;
    fprintf(fout, "\tL1I_total_cache_misses = %llu\n", total_css.misses);
    L1I_total_cache_misses += total_css.misses;
    if (total_css.accesses > 0) {
      fprintf(fout, "\tL1I_total_cache_miss_rate = %.4lf\n",
              (double)total_css.misses / (double)total_css.accesses);
    }
    fprintf(fout, "\tL1I_total_cache_pending_hits = %llu\n",
            total_css.pending_hits);
    L1I_total_cache_pending_hits += total_css.pending_hits;
    fprintf(fout, "\tL1I_total_cache_reservation_fails = %llu\n",
            total_css.res_fails);
    L1I_total_cache_reservation_fails += total_css.res_fails;
  }

  // L1D
  if (!m_shader_config->m_L1D_config.disabled()) {
    total_css.clear();
    css.clear();
    fprintf(fout, "L1D_cache:\n");
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
      m_cluster[i]->get_L1D_sub_stats(css);

      fprintf(stdout,
              "\tL1D_cache_core[%d]: Access = %llu, Miss = %llu, Miss_rate = "
              "%.3lf, Pending_hits = %llu, Reservation_fails = %llu\n",
              i, css.accesses, css.misses,
              (double)css.misses / (double)css.accesses, css.pending_hits,
              css.res_fails);

      total_css += css;
    }
    fprintf(fout, "\tL1D_total_cache_accesses = %llu\n", total_css.accesses);
    L1D_total_cache_accesses += total_css.accesses;
    fprintf(fout, "\tL1D_total_cache_misses = %llu\n", total_css.misses);
    L1D_total_cache_misses += total_css.misses;
    if (total_css.accesses > 0) {
      fprintf(fout, "\tL1D_total_cache_miss_rate = %.4lf\n",
              (double)total_css.misses / (double)total_css.accesses);
    }
    fprintf(fout, "\tL1D_total_cache_pending_hits = %llu\n",
            total_css.pending_hits);
    L1D_total_cache_pending_hits += total_css.pending_hits;
    fprintf(fout, "\tL1D_total_cache_reservation_fails = %llu\n",
            total_css.res_fails);
    L1D_total_cache_reservation_fails = L1D_total_cache_reservation_fails + total_css.res_fails;
    total_css.print_port_stats(fout, "\tL1D_cache");
  }

  // L1C
  if (!m_shader_config->m_L1C_config.disabled()) {
    total_css.clear();
    css.clear();
    fprintf(fout, "L1C_cache:\n");
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; ++i) {
      m_cluster[i]->get_L1C_sub_stats(css);
      total_css += css;
    }
    fprintf(fout, "\tL1C_total_cache_accesses = %llu\n", total_css.accesses);
    L1C_total_cache_accesses += total_css.accesses;
    fprintf(fout, "\tL1C_total_cache_misses = %llu\n", total_css.misses);
    L1C_total_cache_misses += total_css.misses;
    if (total_css.accesses > 0) {
      fprintf(fout, "\tL1C_total_cache_miss_rate = %.4lf\n",
              (double)total_css.misses / (double)total_css.accesses);
    }
    fprintf(fout, "\tL1C_total_cache_pending_hits = %llu\n",
            total_css.pending_hits);
    L1C_total_cache_pending_hits += total_css.pending_hits;
    fprintf(fout, "\tL1C_total_cache_reservation_fails = %llu\n",
            total_css.res_fails);
    L1C_total_cache_reservation_fails += total_css.res_fails;
  }

  // L1T
  if (!m_shader_config->m_L1T_config.disabled()) {
    total_css.clear();
    css.clear();
    fprintf(fout, "L1T_cache:\n");
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; ++i) {
      m_cluster[i]->get_L1T_sub_stats(css);
      total_css += css;
    }
    fprintf(fout, "\tL1T_total_cache_accesses = %llu\n", total_css.accesses);
    L1T_total_cache_accesses += total_css.accesses;
    fprintf(fout, "\tL1T_total_cache_misses = %llu\n", total_css.misses);
    L1T_total_cache_misses += total_css.misses;
    if (total_css.accesses > 0) {
      fprintf(fout, "\tL1T_total_cache_miss_rate = %.4lf\n",
              (double)total_css.misses / (double)total_css.accesses);
    }
    fprintf(fout, "\tL1T_total_cache_pending_hits = %llu\n",
            total_css.pending_hits);
    L1T_total_cache_pending_hits += total_css.pending_hits; 
    fprintf(fout, "\tL1T_total_cache_reservation_fails = %llu\n",
            total_css.res_fails);
    L1T_total_cache_reservation_fails += total_css.res_fails;
  }
}

void gpgpu_sim::shader_print_l1_miss_stat(FILE *fout) const {
  unsigned total_d1_misses = 0, total_d1_accesses = 0;
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; ++i) {
    unsigned custer_d1_misses = 0, cluster_d1_accesses = 0;
    m_cluster[i]->print_cache_stats(fout, cluster_d1_accesses,
                                    custer_d1_misses);
    total_d1_misses += custer_d1_misses;
    total_dL1_misses += custer_d1_misses;
    total_d1_accesses += cluster_d1_accesses;
    total_dL1_accesses += cluster_d1_accesses;
  }
  fprintf(fout, "total_dl1_misses=%d\n", total_d1_misses);
  fprintf(fout, "total_dl1_accesses=%d\n", total_d1_accesses);
  fprintf(fout, "total_dl1_miss_rate= %f\n",
          (float)total_d1_misses / (float)total_d1_accesses);
  /*
  fprintf(fout, "THD_INSN_AC: ");
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++)
     fprintf(fout, "%d ", m_sc[0]->get_thread_n_insn_ac(i));
  fprintf(fout, "\n");
  fprintf(fout, "T_L1_Mss: "); //l1 miss rate per thread
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++)
     fprintf(fout, "%d ", m_sc[0]->get_thread_n_l1_mis_ac(i));
  fprintf(fout, "\n");
  fprintf(fout, "T_L1_Mgs: "); //l1 merged miss rate per thread
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++)
     fprintf(fout, "%d ", m_sc[0]->get_thread_n_l1_mis_ac(i) -
  m_sc[0]->get_thread_n_l1_mrghit_ac(i)); fprintf(fout, "\n"); fprintf(fout,
  "T_L1_Acc: "); //l1 access per thread for (unsigned i=0;
  i<m_shader_config->n_thread_per_shader; i++) fprintf(fout, "%d ",
  m_sc[0]->get_thread_n_l1_access_ac(i)); fprintf(fout, "\n");

  //per warp
  int temp =0;
  fprintf(fout, "W_L1_Mss: "); //l1 miss rate per warp
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++) {
     temp += m_sc[0]->get_thread_n_l1_mis_ac(i);
     if (i%m_shader_config->warp_size ==
  (unsigned)(m_shader_config->warp_size-1)) { fprintf(fout, "%d ", temp); temp =
  0;
     }
  }
  fprintf(fout, "\n");
  temp=0;
  fprintf(fout, "W_L1_Mgs: "); //l1 merged miss rate per warp
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++) {
     temp += (m_sc[0]->get_thread_n_l1_mis_ac(i) -
  m_sc[0]->get_thread_n_l1_mrghit_ac(i) ); if (i%m_shader_config->warp_size ==
  (unsigned)(m_shader_config->warp_size-1)) { fprintf(fout, "%d ", temp); temp =
  0;
     }
  }
  fprintf(fout, "\n");
  temp =0;
  fprintf(fout, "W_L1_Acc: "); //l1 access per warp
  for (unsigned i=0; i<m_shader_config->n_thread_per_shader; i++) {
     temp += m_sc[0]->get_thread_n_l1_access_ac(i);
     if (i%m_shader_config->warp_size ==
  (unsigned)(m_shader_config->warp_size-1)) { fprintf(fout, "%d ", temp); temp =
  0;
     }
  }
  fprintf(fout, "\n");
  */
}

void warp_inst_t::print(FILE *fout) const {
  if (empty()) {
    fprintf(fout, "bubble\n");
    return;
  } else
    fprintf(fout, "0x%04x ", pc);
  fprintf(fout, "w%02d[", m_warp_id);
  for (unsigned j = 0; j < m_config->warp_size; j++)
    fprintf(fout, "%c", (active(j) ? '1' : '0'));
  fprintf(fout, "]: ");
  m_config->gpgpu_ctx->func_sim->ptx_print_insn(pc, fout);
  fprintf(fout, "\n");
}
void shader_core_ctx::incexecstat(warp_inst_t *&inst) {
  if (inst->mem_op == TEX) inctex_stat(inst->active_count(), 1);

  // Latency numbers for next operations are used to scale the power values
  // for special operations, according observations from microbenchmarking
  // TODO: put these numbers in the xml configuration

  switch (inst->sp_op) {
    case INT__OP:
      incialu_stat(inst->active_count(), 32);
      break;
    case INT_MUL_OP:
      incimul_stat(inst->active_count(), 7.2);
      break;
    case INT_MUL24_OP:
      incimul24_stat(inst->active_count(), 4.2);
      break;
    case INT_MUL32_OP:
      incimul32_stat(inst->active_count(), 4);
      break;
    case INT_DIV_OP:
      incidiv_stat(inst->active_count(), 40);
      break;
    case FP__OP:
      incfpalu_stat(inst->active_count(), 1);
      break;
    case FP_MUL_OP:
      incfpmul_stat(inst->active_count(), 1.8);
      break;
    case FP_DIV_OP:
      incfpdiv_stat(inst->active_count(), 48);
      break;
    case FP_SQRT_OP:
      inctrans_stat(inst->active_count(), 25);
      break;
    case FP_LG_OP:
      inctrans_stat(inst->active_count(), 35);
      break;
    case FP_SIN_OP:
      inctrans_stat(inst->active_count(), 12);
      break;
    case FP_EXP_OP:
      inctrans_stat(inst->active_count(), 35);
      break;
    default:
      break;
  }
}
void shader_core_ctx::print_stage(unsigned int stage, FILE *fout) const {
  m_pipeline_reg[stage].print(fout);
  // m_pipeline_reg[stage].print(fout);
}

void shader_core_ctx::display_simt_state(FILE *fout, int mask) const {
  if ((mask & 4) && m_config->model == POST_DOMINATOR) {
    fprintf(fout, "per warp SIMT control-flow state:\n");
    unsigned n = m_config->n_thread_per_shader / m_config->warp_size;
    for (unsigned i = 0; i < n; i++) {
      unsigned nactive = 0;
      for (unsigned j = 0; j < m_config->warp_size; j++) {
        unsigned tid = i * m_config->warp_size + j;
        int done = ptx_thread_done(tid);
        nactive += (ptx_thread_done(tid) ? 0 : 1);
        if (done && (mask & 8)) {
          unsigned done_cycle = m_thread[tid]->donecycle();
          if (done_cycle) {
            printf("\n w%02u:t%03u: done @ cycle %u", i, tid, done_cycle);
          }
        }
      }
      if (nactive == 0) {
        continue;
      }
      m_simt_stack[i]->print(fout);
    }
    fprintf(fout, "\n");
  }
}

void ldst_unit::print(FILE *fout) const {
  fprintf(fout, "LD/ST unit  = ");
  m_dispatch_reg->print(fout);
  if (m_mem_rc != NO_RC_FAIL) {
    fprintf(fout, "              LD/ST stall condition: ");
    switch (m_mem_rc) {
      case BK_CONF:
        fprintf(fout, "BK_CONF");
        break;
      case MSHR_RC_FAIL:
        fprintf(fout, "MSHR_RC_FAIL");
        break;
      case ICNT_RC_FAIL:
        fprintf(fout, "ICNT_RC_FAIL");
        break;
      case COAL_STALL:
        fprintf(fout, "COAL_STALL");
        break;
      case WB_ICNT_RC_FAIL:
        fprintf(fout, "WB_ICNT_RC_FAIL");
        break;
      case WB_CACHE_RSRV_FAIL:
        fprintf(fout, "WB_CACHE_RSRV_FAIL");
        break;
      case N_MEM_STAGE_STALL_TYPE:
        fprintf(fout, "N_MEM_STAGE_STALL_TYPE");
        break;
      default:
        abort();
    }
    fprintf(fout, "\n");
  }
  fprintf(fout, "LD/ST wb    = ");
  m_next_wb.print(fout);
  fprintf(
      fout,
      "Last LD/ST writeback @ %llu + %llu (gpu_sim_cycle+gpu_tot_sim_cycle)\n",
      m_last_inst_gpu_sim_cycle, m_last_inst_gpu_tot_sim_cycle);
  fprintf(fout, "Pending register writes:\n");
  std::map<unsigned /*warp_id*/,
           std::map<unsigned /*regnum*/, unsigned /*count*/> >::const_iterator
      w;
  for (w = m_pending_writes.begin(); w != m_pending_writes.end(); w++) {
    unsigned warp_id = w->first;
    const std::map<unsigned /*regnum*/, unsigned /*count*/> &warp_info =
        w->second;
    if (warp_info.empty()) continue;
    fprintf(fout, "  w%2u : ", warp_id);
    std::map<unsigned /*regnum*/, unsigned /*count*/>::const_iterator r;
    for (r = warp_info.begin(); r != warp_info.end(); ++r) {
      fprintf(fout, "  %u(%u)", r->first, r->second);
    }
    fprintf(fout, "\n");
  }
  m_L1C->display_state(fout);
  m_L1T->display_state(fout);
  if (!m_config->m_L1D_config.disabled()) m_L1D->display_state(fout);
  fprintf(fout, "LD/ST response FIFO (occupancy = %zu):\n",
          m_response_fifo.size());
  for (std::list<mem_fetch *>::const_iterator i = m_response_fifo.begin();
       i != m_response_fifo.end(); i++) {
    const mem_fetch *mf = *i;
    mf->print(fout);
  }
}

void shader_core_ctx::display_pipeline(FILE *fout, int print_mem,
                                       int mask) const {
  fprintf(fout, "=================================================\n");
  fprintf(fout, "shader %u at cycle %Lu+%Lu (%u threads running)\n", m_sid,
          m_gpu->gpu_tot_sim_cycle, m_gpu->gpu_sim_cycle, m_not_completed);
  fprintf(fout, "=================================================\n");

  dump_warp_state(fout);
  fprintf(fout, "\n");

  m_L1I->display_state(fout);

  fprintf(fout, "IF/ID       = ");
  if (!m_inst_fetch_buffer.m_valid)
    fprintf(fout, "bubble\n");
  else {
    fprintf(fout, "w%2u : pc = 0x%x, nbytes = %u\n",
            m_inst_fetch_buffer.m_warp_id, m_inst_fetch_buffer.m_pc,
            m_inst_fetch_buffer.m_nbytes);
  }
  fprintf(fout, "\nibuffer status:\n");
  for (unsigned i = 0; i < m_config->max_warps_per_shader; i++) {
    if (!m_warp[i]->ibuffer_empty()) m_warp[i]->print_ibuffer(fout);
  }
  fprintf(fout, "\n");
  display_simt_state(fout, mask);
  fprintf(fout, "-------------------------- Scoreboard\n");
  m_scoreboard->printContents();
  /*
     fprintf(fout,"ID/OC (SP)  = ");
     print_stage(ID_OC_SP, fout);
     fprintf(fout,"ID/OC (SFU) = ");
     print_stage(ID_OC_SFU, fout);
     fprintf(fout,"ID/OC (MEM) = ");
     print_stage(ID_OC_MEM, fout);
  */
  fprintf(fout, "-------------------------- OP COL\n");
  m_operand_collector.dump(fout);
  /* fprintf(fout, "OC/EX (SP)  = ");
     print_stage(OC_EX_SP, fout);
     fprintf(fout, "OC/EX (SFU) = ");
     print_stage(OC_EX_SFU, fout);
     fprintf(fout, "OC/EX (MEM) = ");
     print_stage(OC_EX_MEM, fout);
  */
  fprintf(fout, "-------------------------- Pipe Regs\n");

  for (unsigned i = 0; i < N_PIPELINE_STAGES; i++) {
    fprintf(fout, "--- %s ---\n", pipeline_stage_name_decode[i]);
    print_stage(i, fout);
    fprintf(fout, "\n");
  }

  fprintf(fout, "-------------------------- Fu\n");
  for (unsigned n = 0; n < m_num_function_units; n++) {
    m_fu[n]->print(fout);
    fprintf(fout, "---------------\n");
  }
  fprintf(fout, "-------------------------- other:\n");

  for (unsigned i = 0; i < num_result_bus; i++) {
    std::string bits = m_result_bus[i]->to_string();
    fprintf(fout, "EX/WB sched[%d]= %s\n", i, bits.c_str());
  }
  fprintf(fout, "EX/WB      = ");
  print_stage(EX_WB, fout);
  fprintf(fout, "\n");
  fprintf(
      fout,
      "Last EX/WB writeback @ %llu + %llu (gpu_sim_cycle+gpu_tot_sim_cycle)\n",
      m_last_inst_gpu_sim_cycle, m_last_inst_gpu_tot_sim_cycle);

  if (m_active_threads.count() <= 2 * m_config->warp_size) {
    fprintf(fout, "Active Threads : ");
    unsigned last_warp_id = -1;
    for (unsigned tid = 0; tid < m_active_threads.size(); tid++) {
      unsigned warp_id = tid / m_config->warp_size;
      if (m_active_threads.test(tid)) {
        if (warp_id != last_warp_id) {
          fprintf(fout, "\n  warp %u : ", warp_id);
          last_warp_id = warp_id;
        }
        fprintf(fout, "%u ", tid);
      }
    }
  }
}

unsigned int shader_core_config::max_cta(const kernel_info_t &k) const {
  unsigned threads_per_cta = k.threads_per_cta();
  const class function_info *kernel = k.entry();
  unsigned int padded_cta_size = threads_per_cta;
  if (padded_cta_size % warp_size)
    padded_cta_size = ((padded_cta_size / warp_size) + 1) * (warp_size);

  // Limit by n_threads/shader
  unsigned int result_thread = n_thread_per_shader / padded_cta_size;

  const struct gpgpu_ptx_sim_info *kernel_info = ptx_sim_kernel_info(kernel);

  // Limit by shmem/shader
  unsigned int result_shmem = (unsigned)-1;
  if (kernel_info->smem > 0)
    result_shmem = gpgpu_shmem_size / kernel_info->smem;

  // Limit by register count, rounded up to multiple of 4.
  unsigned int result_regs = (unsigned)-1;
  if (kernel_info->regs > 0)
    result_regs = gpgpu_shader_registers /
                  (padded_cta_size * ((kernel_info->regs + 3) & ~3));

  // Limit by CTA
  unsigned int result_cta = max_cta_per_core;

  unsigned result = result_thread;
  result = gs_min2(result, result_shmem);
  result = gs_min2(result, result_regs);
  result = gs_min2(result, result_cta);

  static const struct gpgpu_ptx_sim_info *last_kinfo = NULL;
  if (last_kinfo !=
      kernel_info) {  // Only print out stats if kernel_info struct changes
    last_kinfo = kernel_info;
    printf("GPGPU-Sim uArch: CTA/core = %u, limited by:", result);
    if (result == result_thread) printf(" threads");
    if (result == result_shmem) printf(" shmem");
    if (result == result_regs) printf(" regs");
    if (result == result_cta) printf(" cta_limit");
    printf("\n");
  }

  // gpu_max_cta_per_shader is limited by number of CTAs if not enough to keep
  // all cores busy
  if (k.num_blocks() < result * num_shader()) {
    result = k.num_blocks() / num_shader();
    if (k.num_blocks() % num_shader()) result++;
  }

  assert(result <= MAX_CTA_PER_SHADER);
  if (result < 1) {
    printf(
        "GPGPU-Sim uArch: ERROR ** Kernel requires more resources than shader "
        "has.\n");
    if (gpgpu_ignore_resources_limitation) {
      printf(
          "GPGPU-Sim uArch: gpgpu_ignore_resources_limitation is set, ignore "
          "the ERROR!\n");
      return 1;
    }
    abort();
  }

  if (adaptive_cache_config && !k.cache_config_set) {
    // For more info about adaptive cache, see
    // https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#shared-memory-7-x
    unsigned total_shmed = kernel_info->smem * result;
    assert(total_shmed >= 0 && total_shmed <= gpgpu_shmem_size);
    // assert(gpgpu_shmem_size == 98304); //Volta has 96 KB shared
    // assert(m_L1D_config.get_nset() == 4);  //Volta L1 has four sets
    if (total_shmed < gpgpu_shmem_size) {
      switch (adaptive_cache_config) {
        case FIXED:
          break;
        case ADAPTIVE_VOLTA: {
          // For Volta, we assign the remaining shared memory to L1 cache
          // For more info about adaptive cache, see
          // https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#shared-memory-7-x
          // assert(gpgpu_shmem_size == 98304); //Volta has 96 KB shared

          // To Do: make it flexible and not tuned to 9KB share memory
          unsigned max_assoc = m_L1D_config.get_max_assoc();
          if (total_shmed == 0)
            m_L1D_config.set_assoc(max_assoc);  // L1 is 128KB and shd=0
          else if (total_shmed > 0 && total_shmed <= 8192)
            m_L1D_config.set_assoc(0.9375 *
                                   max_assoc);  // L1 is 120KB and shd=8KB
          else if (total_shmed > 8192 && total_shmed <= 16384)
            m_L1D_config.set_assoc(0.875 *
                                   max_assoc);  // L1 is 112KB and shd=16KB
          else if (total_shmed > 16384 && total_shmed <= 32768)
            m_L1D_config.set_assoc(0.75 * max_assoc);  // L1 is 96KB and
                                                       // shd=32KB
          else if (total_shmed > 32768 && total_shmed <= 65536)
            m_L1D_config.set_assoc(0.5 * max_assoc);  // L1 is 64KB and shd=64KB
          else if (total_shmed > 65536 && total_shmed <= gpgpu_shmem_size)
            m_L1D_config.set_assoc(0.25 * max_assoc);  // L1 is 32KB and
                                                       // shd=96KB
          else
            assert(0);
          break;
        }
        default:
          assert(0);
      }

      printf("GPGPU-Sim: Reconfigure L1 cache to %uKB\n",
             m_L1D_config.get_total_size_inKB());
    }

    k.cache_config_set = true;
  }

  return result;
}

void shader_core_config::set_pipeline_latency() {
  // calculate the max latency  based on the input

  unsigned int_latency[6];
  unsigned fp_latency[5];
  unsigned dp_latency[5];
  unsigned sfu_latency;
  unsigned tensor_latency;

  /*
   * [0] ADD,SUB
   * [1] MAX,Min
   * [2] MUL
   * [3] MAD
   * [4] DIV
   * [5] SHFL
   */
  sscanf(gpgpu_ctx->func_sim->opcode_latency_int, "%u,%u,%u,%u,%u,%u",
         &int_latency[0], &int_latency[1], &int_latency[2], &int_latency[3],
         &int_latency[4], &int_latency[5]);
  sscanf(gpgpu_ctx->func_sim->opcode_latency_fp, "%u,%u,%u,%u,%u",
         &fp_latency[0], &fp_latency[1], &fp_latency[2], &fp_latency[3],
         &fp_latency[4]);
  sscanf(gpgpu_ctx->func_sim->opcode_latency_dp, "%u,%u,%u,%u,%u",
         &dp_latency[0], &dp_latency[1], &dp_latency[2], &dp_latency[3],
         &dp_latency[4]);
  sscanf(gpgpu_ctx->func_sim->opcode_latency_sfu, "%u", &sfu_latency);
  sscanf(gpgpu_ctx->func_sim->opcode_latency_tensor, "%u", &tensor_latency);

  // all div operation are executed on sfu
  // assume that the max latency are dp div or normal sfu_latency
  max_sfu_latency = std::max(dp_latency[4], sfu_latency);
  // assume that the max operation has the max latency
  max_sp_latency = fp_latency[1];
  max_int_latency = std::max(int_latency[1], int_latency[5]);
  max_dp_latency = dp_latency[1];
  max_tensor_core_latency = tensor_latency;
}

void shader_core_ctx::cycle() {
  if (!isactive() && get_not_completed() == 0) return;

  m_stats->shader_cycles[m_sid]++;
  writeback();
  execute();
  read_operands();
  issue();
  for (int i = 0; i < m_config->inst_fetch_throughput; ++i) {
    decode();
    fetch();
  }
}

// Flushes all content of the cache to memory

void shader_core_ctx::cache_flush() { m_ldst_unit->flush(); }

void shader_core_ctx::cache_invalidate() { m_ldst_unit->invalidate(); }

// modifiers
std::list<opndcoll_rfu_t::op_t> opndcoll_rfu_t::arbiter_t::allocate_reads() {
  std::list<op_t>
      result;  // a list of registers that (a) are in different register banks,
               // (b) do not go to the same operand collector

  int input;
  int output;
  int _inputs = m_num_banks;
  int _outputs = m_num_collectors;
  int _square = (_inputs > _outputs) ? _inputs : _outputs;
  assert(_square > 0);
  int _pri = (int)m_last_cu;

  // Clear matching
  for (int i = 0; i < _inputs; ++i) _inmatch[i] = -1;
  for (int j = 0; j < _outputs; ++j) _outmatch[j] = -1;

  for (unsigned i = 0; i < m_num_banks; i++) {
    for (unsigned j = 0; j < m_num_collectors; j++) {
      assert(i < (unsigned)_inputs);
      assert(j < (unsigned)_outputs);
      _request[i][j] = 0;
    }
    if (!m_queue[i].empty()) {
      const op_t &op = m_queue[i].front();
      int oc_id = op.get_oc_id();
      assert(i < (unsigned)_inputs);
      assert(oc_id < _outputs);
      _request[i][oc_id] = 1;
    }
    if (m_allocated_bank[i].is_write()) {
      assert(i < (unsigned)_inputs);
      _inmatch[i] = 0;  // write gets priority
    }
  }

  ///// wavefront allocator from booksim... --->

  // Loop through diagonals of request matrix
  // printf("####\n");

  for (int p = 0; p < _square; ++p) {
    output = (_pri + p) % _outputs;

    // Step through the current diagonal
    for (input = 0; input < _inputs; ++input) {
      assert(input < _inputs);
      assert(output < _outputs);
      if ((output < _outputs) && (_inmatch[input] == -1) &&
          //( _outmatch[output] == -1 ) &&   //allow OC to read multiple reg
          // banks at the same cycle
          (_request[input][output] /*.label != -1*/)) {
        // Grant!
        _inmatch[input] = output;
        _outmatch[output] = input;
        // printf("Register File: granting bank %d to OC %d, schedid %d, warpid
        // %d, Regid %d\n", input, output, (m_queue[input].front()).get_sid(),
        // (m_queue[input].front()).get_wid(),
        // (m_queue[input].front()).get_reg());
      }

      output = (output + 1) % _outputs;
    }
  }

  // Round-robin the priority diagonal
  _pri = (_pri + 1) % _outputs;

  /// <--- end code from booksim

  m_last_cu = _pri;
  for (unsigned i = 0; i < m_num_banks; i++) {
    if (_inmatch[i] != -1) {
      if (!m_allocated_bank[i].is_write()) {
        unsigned bank = (unsigned)i;
        op_t &op = m_queue[bank].front();
        result.push_back(op);
        m_queue[bank].pop_front();
      }
    }
  }

  return result;
}

barrier_set_t::barrier_set_t(shader_core_ctx *shader,
                             unsigned max_warps_per_core,
                             unsigned max_cta_per_core,
                             unsigned max_barriers_per_cta,
                             unsigned warp_size) {
  m_max_warps_per_core = max_warps_per_core;
  m_max_cta_per_core = max_cta_per_core;
  m_max_barriers_per_cta = max_barriers_per_cta;
  m_warp_size = warp_size;
  m_shader = shader;
  if (max_warps_per_core > WARP_PER_CTA_MAX) {
    printf(
        "ERROR ** increase WARP_PER_CTA_MAX in shader.h from %u to >= %u or "
        "warps per cta in gpgpusim.config\n",
        WARP_PER_CTA_MAX, max_warps_per_core);
    exit(1);
  }
  if (max_barriers_per_cta > MAX_BARRIERS_PER_CTA) {
    printf(
        "ERROR ** increase MAX_BARRIERS_PER_CTA in abstract_hardware_model.h "
        "from %u to >= %u or barriers per cta in gpgpusim.config\n",
        MAX_BARRIERS_PER_CTA, max_barriers_per_cta);
    exit(1);
  }
  m_warp_active.reset();
  m_warp_at_barrier.reset();
  for (unsigned i = 0; i < max_barriers_per_cta; i++) {
    m_bar_id_to_warps[i].reset();
  }
}

// during cta allocation
void barrier_set_t::allocate_barrier(unsigned cta_id, warp_set_t warps) {
  assert(cta_id < m_max_cta_per_core);
  cta_to_warp_t::iterator w = m_cta_to_warps.find(cta_id);
  assert(w == m_cta_to_warps.end());  // cta should not already be active or
                                      // allocated barrier resources
  m_cta_to_warps[cta_id] = warps;
  assert(m_cta_to_warps.size() <=
         m_max_cta_per_core);  // catch cta's that were not properly deallocated

  m_warp_active |= warps;
  m_warp_at_barrier &= ~warps;
  for (unsigned i = 0; i < m_max_barriers_per_cta; i++) {
    m_bar_id_to_warps[i] &= ~warps;
  }
}

// during cta deallocation
void barrier_set_t::deallocate_barrier(unsigned cta_id) {
  cta_to_warp_t::iterator w = m_cta_to_warps.find(cta_id);
  if (w == m_cta_to_warps.end()) return;
  warp_set_t warps = w->second;
  warp_set_t at_barrier = warps & m_warp_at_barrier;
  assert(at_barrier.any() == false);  // no warps stuck at barrier
  warp_set_t active = warps & m_warp_active;
  assert(active.any() == false);  // no warps in CTA still running
  m_warp_active &= ~warps;
  m_warp_at_barrier &= ~warps;

  for (unsigned i = 0; i < m_max_barriers_per_cta; i++) {
    warp_set_t at_a_specific_barrier = warps & m_bar_id_to_warps[i];
    assert(at_a_specific_barrier.any() == false);  // no warps stuck at barrier
    m_bar_id_to_warps[i] &= ~warps;
  }
  m_cta_to_warps.erase(w);
}

// individual warp hits barrier
void barrier_set_t::warp_reaches_barrier(unsigned cta_id, unsigned warp_id,
                                         warp_inst_t *inst) {
  barrier_type bar_type = inst->bar_type;
  unsigned bar_id = inst->bar_id;
  unsigned bar_count = inst->bar_count;
  assert(bar_id != (unsigned)-1);
  cta_to_warp_t::iterator w = m_cta_to_warps.find(cta_id);

  if (w == m_cta_to_warps.end()) {  // cta is active
    printf(
        "ERROR ** cta_id %u not found in barrier set on cycle %llu+%llu...\n",
        cta_id, m_shader->get_gpu()->gpu_tot_sim_cycle,
        m_shader->get_gpu()->gpu_sim_cycle);
    dump();
    abort();
  }
  assert(w->second.test(warp_id) == true);  // warp is in cta

  m_bar_id_to_warps[bar_id].set(warp_id);
  if (bar_type == SYNC || bar_type == RED) {
    m_warp_at_barrier.set(warp_id);
  }
  warp_set_t warps_in_cta = w->second;
  warp_set_t at_barrier = warps_in_cta & m_bar_id_to_warps[bar_id];
  warp_set_t active = warps_in_cta & m_warp_active;
  if (bar_count == (unsigned)-1) {
    if (at_barrier == active) {
      // all warps have reached barrier, so release waiting warps...
      m_bar_id_to_warps[bar_id] &= ~at_barrier;
      m_warp_at_barrier &= ~at_barrier;
      if (bar_type == RED) {
        m_shader->broadcast_barrier_reduction(cta_id, bar_id, at_barrier);
      }
    }
  } else {
    // TODO: check on the hardware if the count should include warp that exited
    if ((at_barrier.count() * m_warp_size) == bar_count) {
      // required number of warps have reached barrier, so release waiting
      // warps...
      m_bar_id_to_warps[bar_id] &= ~at_barrier;
      m_warp_at_barrier &= ~at_barrier;
      if (bar_type == RED) {
        m_shader->broadcast_barrier_reduction(cta_id, bar_id, at_barrier);
      }
    }
  }
}

// warp reaches exit
void barrier_set_t::warp_exit(unsigned warp_id) {
  // caller needs to verify all threads in warp are done, e.g., by checking PDOM
  // stack to see it has only one entry during exit_impl()
  m_warp_active.reset(warp_id);

  // test for barrier release
  cta_to_warp_t::iterator w = m_cta_to_warps.begin();
  for (; w != m_cta_to_warps.end(); ++w) {
    if (w->second.test(warp_id) == true) break;
  }
  warp_set_t warps_in_cta = w->second;
  warp_set_t active = warps_in_cta & m_warp_active;

  for (unsigned i = 0; i < m_max_barriers_per_cta; i++) {
    warp_set_t at_a_specific_barrier = warps_in_cta & m_bar_id_to_warps[i];
    if (at_a_specific_barrier == active) {
      // all warps have reached barrier, so release waiting warps...
      m_bar_id_to_warps[i] &= ~at_a_specific_barrier;
      m_warp_at_barrier &= ~at_a_specific_barrier;
    }
  }
}

// assertions
bool barrier_set_t::warp_waiting_at_barrier(unsigned warp_id) const {
  return m_warp_at_barrier.test(warp_id);
}

void barrier_set_t::dump() {
  printf("barrier set information\n");
  printf("  m_max_cta_per_core = %u\n", m_max_cta_per_core);
  printf("  m_max_warps_per_core = %u\n", m_max_warps_per_core);
  printf(" m_max_barriers_per_cta =%u\n", m_max_barriers_per_cta);
  printf("  cta_to_warps:\n");

  cta_to_warp_t::const_iterator i;
  for (i = m_cta_to_warps.begin(); i != m_cta_to_warps.end(); i++) {
    unsigned cta_id = i->first;
    warp_set_t warps = i->second;
    printf("    cta_id %u : %s\n", cta_id, warps.to_string().c_str());
  }
  printf("  warp_active: %s\n", m_warp_active.to_string().c_str());
  printf("  warp_at_barrier: %s\n", m_warp_at_barrier.to_string().c_str());
  for (unsigned i = 0; i < m_max_barriers_per_cta; i++) {
    warp_set_t warps_reached_barrier = m_bar_id_to_warps[i];
    printf("  warp_at_barrier %u: %s\n", i,
           warps_reached_barrier.to_string().c_str());
  }
  fflush(stdout);
}

void shader_core_ctx::warp_exit(unsigned warp_id) {
  bool done = true;
  for (unsigned i = warp_id * get_config()->warp_size;
       i < (warp_id + 1) * get_config()->warp_size; i++) {
    //		if(this->m_thread[i]->m_functional_model_thread_state &&
    // this->m_thread[i].m_functional_model_thread_state->donecycle()==0) {
    // done = false;
    //		}

    if (m_thread[i] && !m_thread[i]->is_done()) done = false;
  }
  // if (m_warp[warp_id].get_n_completed() == get_config()->warp_size)
  // if (this->m_simt_stack[warp_id]->get_num_entries() == 0)
  if (done) m_barriers.warp_exit(warp_id);
}

bool shader_core_ctx::check_if_non_released_reduction_barrier(
    warp_inst_t &inst) {
  unsigned warp_id = inst.warp_id();
  bool bar_red_op = (inst.op == BARRIER_OP) && (inst.bar_type == RED);
  bool non_released_barrier_reduction = false;
  bool warp_stucked_at_barrier = warp_waiting_at_barrier(warp_id);
  bool single_inst_in_pipeline =
      (m_warp[warp_id]->num_issued_inst_in_pipeline() == 1);
  non_released_barrier_reduction =
      single_inst_in_pipeline and warp_stucked_at_barrier and bar_red_op;
  printf("non_released_barrier_reduction=%u\n", non_released_barrier_reduction);
  return non_released_barrier_reduction;
}

bool shader_core_ctx::warp_waiting_at_barrier(unsigned warp_id) const {
  return m_barriers.warp_waiting_at_barrier(warp_id);
}

bool shader_core_ctx::warp_waiting_at_mem_barrier(unsigned warp_id) {
  if (!m_warp[warp_id]->get_membar()) return false;
  if (!m_scoreboard->pendingWrites(warp_id, m_config->pending_write_ignore)) {
    m_warp[warp_id]->clear_membar();
    if (m_gpu->get_config().flush_l1()) {
      // Mahmoud fixed this on Nov 2019
      // Invalidate L1 cache
      // Based on Nvidia Doc, at MEM barrier, we have to
      //(1) wait for all pending writes till they are acked
      //(2) invalidate L1 cache to ensure coherence and avoid reading stall data
      cache_invalidate();
      // TO DO: you need to stall the SM for 5k cycles.
    }
    return false;
  }
  return true;
}

void shader_core_ctx::set_max_cta(const kernel_info_t &kernel) {
  // calculate the max cta count and cta size for local memory address mapping
  kernel_max_cta_per_shader = m_config->max_cta(kernel);
  unsigned int gpu_cta_size = kernel.threads_per_cta();
  kernel_padded_threads_per_cta =
      (gpu_cta_size % m_config->warp_size)
          ? m_config->warp_size * ((gpu_cta_size / m_config->warp_size) + 1)
          : gpu_cta_size;
}

void shader_core_ctx::decrement_atomic_count(unsigned wid, unsigned n) {
  assert(m_warp[wid]->get_n_atomic() >= n);
  m_warp[wid]->dec_n_atomic(n);
}

void shader_core_ctx::broadcast_barrier_reduction(unsigned cta_id,
                                                  unsigned bar_id,
                                                  warp_set_t warps) {
  for (unsigned i = 0; i < m_config->max_warps_per_shader; i++) {
    if (warps.test(i)) {
      const warp_inst_t *inst =
          m_warp[i]->restore_info_of_last_inst_at_barrier();
      const_cast<warp_inst_t *>(inst)->broadcast_barrier_reduction(
          inst->get_active_mask());
    }
  }
}

bool shader_core_ctx::fetch_unit_response_buffer_full() const { return false; }

void shader_core_ctx::accept_fetch_response(mem_fetch *mf) {
  mf->set_status(IN_SHADER_FETCHED,
                 m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
  m_L1I->fill(mf, m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
}

bool shader_core_ctx::ldst_unit_response_buffer_full() const {
  return m_ldst_unit->response_buffer_full();
}

void shader_core_ctx::accept_ldst_unit_response(mem_fetch *mf) {
  m_ldst_unit->fill(mf);
}

void shader_core_ctx::store_ack(class mem_fetch *mf) {
  assert(mf->get_type() == WRITE_ACK ||
         (m_config->gpgpu_perfect_mem && mf->get_is_write()));
  unsigned warp_id = mf->get_wid();
  m_warp[warp_id]->dec_store_req();
}

void shader_core_ctx::print_cache_stats(FILE *fp, unsigned &dl1_accesses,
                                        unsigned &dl1_misses) {
  m_ldst_unit->print_cache_stats(fp, dl1_accesses, dl1_misses);
}

void shader_core_ctx::get_cache_stats(cache_stats &cs) {
  // Adds stats from each cache to 'cs'
  cs += m_L1I->get_stats();          // Get L1I stats
  m_ldst_unit->get_cache_stats(cs);  // Get L1D, L1C, L1T stats
}

void shader_core_ctx::get_L1I_sub_stats(struct cache_sub_stats &css) const {
  if (m_L1I) m_L1I->get_sub_stats(css);
}
void shader_core_ctx::get_L1D_sub_stats(struct cache_sub_stats &css) const {
  m_ldst_unit->get_L1D_sub_stats(css);
}
void shader_core_ctx::get_L1C_sub_stats(struct cache_sub_stats &css) const {
  m_ldst_unit->get_L1C_sub_stats(css);
}
void shader_core_ctx::get_L1T_sub_stats(struct cache_sub_stats &css) const {
  m_ldst_unit->get_L1T_sub_stats(css);
}

void shader_core_ctx::get_icnt_power_stats(long &n_simt_to_mem,
                                           long &n_mem_to_simt) const {
  n_simt_to_mem += m_stats->n_simt_to_mem[m_sid];
  n_mem_to_simt += m_stats->n_mem_to_simt[m_sid];
}

bool shd_warp_t::functional_done() const {
  return get_n_completed() == m_warp_size;
}

bool shd_warp_t::hardware_done() const {
  return functional_done() && stores_done() && !inst_in_pipeline();
}

bool shd_warp_t::check_waiting() {
  if (functional_done()) {
    cout <<"functional_done()";
    // waiting to be initialized with a kernel
  } else if (m_shader->warp_waiting_at_barrier(m_warp_id)) {
    cout <<"warp_waiting_at_barrier()";
    // waiting for other warps in CTA to reach barrier
  } else if (m_shader->warp_waiting_at_mem_barrier(m_warp_id)) {
    cout <<"warp_waiting_at_mem_barrier()";
    // waiting for memory barrier
  } else if (m_n_atomic > 0) {
    cout <<"m_n_atomic";
    // waiting for atomic operation to complete at memory:
    // this stall is not required for accurate timing model, but rather we
    // stall here since if a call/return instruction occurs in the meantime
    // the functional execution of the atomic when it hits DRAM can cause
    // the wrong register to be read.
  }
}

bool shd_warp_t::waiting() {
  if (functional_done()) {
    // waiting to be initialized with a kernel
    return true;
  } else if (m_shader->warp_waiting_at_barrier(m_warp_id)) {
    // waiting for other warps in CTA to reach barrier
    return true;
  } else if (m_shader->warp_waiting_at_mem_barrier(m_warp_id)) {
    // waiting for memory barrier
    return true;
  } else if (m_n_atomic > 0) {
    // waiting for atomic operation to complete at memory:
    // this stall is not required for accurate timing model, but rather we
    // stall here since if a call/return instruction occurs in the meantime
    // the functional execution of the atomic when it hits DRAM can cause
    // the wrong register to be read.
    return true;
  }
  return false;
}

bool shd_warp_t::waiting_barrier() {
  if (functional_done()) {
    // waiting to be initialized with a kernel
    return false;
  } else if (m_shader->warp_waiting_at_barrier(m_warp_id)) {
    // waiting for other warps in CTA to reach barrier
    return true;
  } else if (m_shader->warp_waiting_at_mem_barrier(m_warp_id)) {
    // waiting for memory barrier
    return false;
  } else if (m_n_atomic > 0) {
    // waiting for atomic operation to complete at memory:
    // this stall is not required for accurate timing model, but rather we
    // stall here since if a call/return instruction occurs in the meantime
    // the functional execution of the atomic when it hits DRAM can cause
    // the wrong register to be read.
    return true;
  }
  return false;
}

bool shd_warp_t::waiting_fence() {
  if (functional_done()) {
    // waiting to be initialized with a kernel
    return false;
  } else if (m_shader->warp_waiting_at_mem_barrier(m_warp_id)) {
    // waiting for memory barrier
    return true;
  }
  return false;
}

bool shd_warp_t::waiting_idle() {
  if (functional_done()) {
    // waiting to be initialized with a kernel
    return true;
  }
  return false;
}

void shd_warp_t::print(FILE *fout) const {
  if (!done_exit()) {
    fprintf(fout, "w%02u npc: 0x%04x, done:%c%c%c%c:%2u i:%u s:%u a:%u (done: ",
            m_warp_id, m_next_pc, (functional_done() ? 'f' : ' '),
            (stores_done() ? 's' : ' '), (inst_in_pipeline() ? ' ' : 'i'),
            (done_exit() ? 'e' : ' '), n_completed, m_inst_in_pipeline,
            m_stores_outstanding, m_n_atomic);
    for (unsigned i = m_warp_id * m_warp_size;
         i < (m_warp_id + 1) * m_warp_size; i++) {
      if (m_shader->ptx_thread_done(i))
        fprintf(fout, "1");
      else
        fprintf(fout, "0");
      if ((((i + 1) % 4) == 0) && (i + 1) < (m_warp_id + 1) * m_warp_size)
        fprintf(fout, ",");
    }
    fprintf(fout, ") ");
    fprintf(fout, " active=%s", m_active_threads.to_string().c_str());
    fprintf(fout, " last fetched @ %5llu", m_last_fetch);
    if (m_imiss_pending) fprintf(fout, " i-miss pending");
    fprintf(fout, "\n");
  }
}

void shd_warp_t::print_ibuffer(FILE *fout) const {
  fprintf(fout, "  ibuffer[%2u] : ", m_warp_id);
  for (unsigned i = 0; i < IBUFFER_SIZE; i++) {
    const inst_t *inst = m_ibuffer[i].m_inst;
    if (inst)
      inst->print_insn(fout);
    else if (m_ibuffer[i].m_valid)
      fprintf(fout, " <invalid instruction> ");
    else
      fprintf(fout, " <empty> ");
  }
  fprintf(fout, "\n");
}

void opndcoll_rfu_t::add_cu_set(unsigned set_id, unsigned num_cu,
                                unsigned num_dispatch) {
  m_cus[set_id].reserve(num_cu);  // this is necessary to stop pointers in m_cu
                                  // from being invalid do to a resize;
  for (unsigned i = 0; i < num_cu; i++) {
    m_cus[set_id].push_back(collector_unit_t());
    m_cu.push_back(&m_cus[set_id].back());
  }
  // for now each collector set gets dedicated dispatch units.
  for (unsigned i = 0; i < num_dispatch; i++) {
    m_dispatch_units.push_back(dispatch_unit_t(&m_cus[set_id]));
  }
}

void opndcoll_rfu_t::add_port(port_vector_t &input, port_vector_t &output,
                              uint_vector_t cu_sets) {
  // m_num_ports++;
  // m_num_collectors += num_collector_units;
  // m_input.resize(m_num_ports);
  // m_output.resize(m_num_ports);
  // m_num_collector_units.resize(m_num_ports);
  // m_input[m_num_ports-1]=input_port;
  // m_output[m_num_ports-1]=output_port;
  // m_num_collector_units[m_num_ports-1]=num_collector_units;
  m_in_ports.push_back(input_port_t(input, output, cu_sets));
}

void opndcoll_rfu_t::init(unsigned num_banks, shader_core_ctx *shader) {
  m_shader = shader;
  m_arbiter.init(m_cu.size(), num_banks);
  // for( unsigned n=0; n<m_num_ports;n++ )
  //    m_dispatch_units[m_output[n]].init( m_num_collector_units[n] );
  m_num_banks = num_banks;
  m_bank_warp_shift = 0;
  m_warp_size = shader->get_config()->warp_size;
  m_bank_warp_shift = (unsigned)(int)(log(m_warp_size + 0.5) / log(2.0));
  assert((m_bank_warp_shift == 5) || (m_warp_size != 32));

  sub_core_model = shader->get_config()->sub_core_model;
  m_num_warp_sceds = shader->get_config()->gpgpu_num_sched_per_core;
  if (sub_core_model)
    assert(num_banks % shader->get_config()->gpgpu_num_sched_per_core == 0);
  m_num_banks_per_sched =
      num_banks / shader->get_config()->gpgpu_num_sched_per_core;

  for (unsigned j = 0; j < m_cu.size(); j++) {
    m_cu[j]->init(j, num_banks, m_bank_warp_shift, shader->get_config(), this,
                  sub_core_model, m_num_banks_per_sched);
  }
  m_initialized = true;
}

int register_bank(int regnum, int wid, unsigned num_banks,
                  unsigned bank_warp_shift, bool sub_core_model,
                  unsigned banks_per_sched, unsigned sched_id) {
  int bank = regnum;
  if (bank_warp_shift) bank += wid;
  if (sub_core_model) {
    unsigned bank_num = (bank % banks_per_sched) + (sched_id * banks_per_sched);
    assert(bank_num < num_banks);
    return bank_num;
  } else
    return bank % num_banks;
}

bool opndcoll_rfu_t::writeback(warp_inst_t &inst) {
  assert(!inst.empty());
  std::list<unsigned> regs = m_shader->get_regs_written(inst);
  for (unsigned op = 0; op < MAX_REG_OPERANDS; op++) {
    int reg_num = inst.arch_reg.dst[op];  // this math needs to match that used
                                          // in function_info::ptx_decode_inst
    if (reg_num >= 0) {                   // valid register
      unsigned bank = register_bank(reg_num, inst.warp_id(), m_num_banks,
                                    m_bank_warp_shift, sub_core_model,
                                    m_num_banks_per_sched, inst.get_schd_id());
      if (m_arbiter.bank_idle(bank)) {
        m_arbiter.allocate_bank_for_write(
            bank,
            op_t(&inst, reg_num, m_num_banks, m_bank_warp_shift, sub_core_model,
                 m_num_banks_per_sched, inst.get_schd_id()));
        inst.arch_reg.dst[op] = -1;
      } else {
        return false;
      }
    }
  }
  for (unsigned i = 0; i < (unsigned)regs.size(); i++) {
    if (m_shader->get_config()->gpgpu_clock_gated_reg_file) {
      unsigned active_count = 0;
      for (unsigned i = 0; i < m_shader->get_config()->warp_size;
           i = i + m_shader->get_config()->n_regfile_gating_group) {
        for (unsigned j = 0; j < m_shader->get_config()->n_regfile_gating_group;
             j++) {
          if (inst.get_active_mask().test(i + j)) {
            active_count += m_shader->get_config()->n_regfile_gating_group;
            break;
          }
        }
      }
      m_shader->incregfile_writes(active_count);
    } else {
      m_shader->incregfile_writes(
          m_shader->get_config()->warp_size);  // inst.active_count());
    }
  }
  return true;
}

void opndcoll_rfu_t::dispatch_ready_cu() {
  for (unsigned p = 0; p < m_dispatch_units.size(); ++p) {
    dispatch_unit_t &du = m_dispatch_units[p];
    collector_unit_t *cu = du.find_ready();
    if (cu) {
      for (unsigned i = 0; i < (cu->get_num_operands() - cu->get_num_regs());
           i++) {
        if (m_shader->get_config()->gpgpu_clock_gated_reg_file) {
          unsigned active_count = 0;
          for (unsigned i = 0; i < m_shader->get_config()->warp_size;
               i = i + m_shader->get_config()->n_regfile_gating_group) {
            for (unsigned j = 0;
                 j < m_shader->get_config()->n_regfile_gating_group; j++) {
              if (cu->get_active_mask().test(i + j)) {
                active_count += m_shader->get_config()->n_regfile_gating_group;
                break;
              }
            }
          }
          m_shader->incnon_rf_operands(active_count);
        } else {
          m_shader->incnon_rf_operands(
              m_shader->get_config()->warp_size);  // cu->get_active_count());
        }
      }
      cu->dispatch();
    }
  }
}

void opndcoll_rfu_t::allocate_cu(unsigned port_num) {
  input_port_t &inp = m_in_ports[port_num];
  for (unsigned i = 0; i < inp.m_in.size(); i++) {
    if ((*inp.m_in[i]).has_ready()) {
      // find a free cu
      for (unsigned j = 0; j < inp.m_cu_sets.size(); j++) {
        std::vector<collector_unit_t> &cu_set = m_cus[inp.m_cu_sets[j]];
        bool allocated = false;
        for (unsigned k = 0; k < cu_set.size(); k++) {
          if (cu_set[k].is_free()) {
            collector_unit_t *cu = &cu_set[k];
            allocated = cu->allocate(inp.m_in[i], inp.m_out[i]);
            m_arbiter.add_read_requests(cu);
            break;
          }
        }
        if (allocated) break;  // cu has been allocated, no need to search more.
      }
      break;  // can only service a single input, if it failed it will fail for
              // others.
    }
  }
}

void opndcoll_rfu_t::allocate_reads() {
  // process read requests that do not have conflicts
  std::list<op_t> allocated = m_arbiter.allocate_reads();
  std::map<unsigned, op_t> read_ops;
  for (std::list<op_t>::iterator r = allocated.begin(); r != allocated.end();
       r++) {
    const op_t &rr = *r;
    unsigned reg = rr.get_reg();
    unsigned wid = rr.get_wid();
    unsigned bank =
        register_bank(reg, wid, m_num_banks, m_bank_warp_shift, sub_core_model,
                      m_num_banks_per_sched, rr.get_sid());
    m_arbiter.allocate_for_read(bank, rr);
    read_ops[bank] = rr;
  }
  std::map<unsigned, op_t>::iterator r;
  for (r = read_ops.begin(); r != read_ops.end(); ++r) {
    op_t &op = r->second;
    unsigned cu = op.get_oc_id();
    unsigned operand = op.get_operand();
    m_cu[cu]->collect_operand(operand);
    if (m_shader->get_config()->gpgpu_clock_gated_reg_file) {
      unsigned active_count = 0;
      for (unsigned i = 0; i < m_shader->get_config()->warp_size;
           i = i + m_shader->get_config()->n_regfile_gating_group) {
        for (unsigned j = 0; j < m_shader->get_config()->n_regfile_gating_group;
             j++) {
          if (op.get_active_mask().test(i + j)) {
            active_count += m_shader->get_config()->n_regfile_gating_group;
            break;
          }
        }
      }
      m_shader->incregfile_reads(active_count);
    } else {
      m_shader->incregfile_reads(
          m_shader->get_config()->warp_size);  // op.get_active_count());
    }
  }
}

bool opndcoll_rfu_t::collector_unit_t::ready() const {
  return (!m_free) && m_not_ready.none() && (*m_output_register).has_free();
}

void opndcoll_rfu_t::collector_unit_t::dump(
    FILE *fp, const shader_core_ctx *shader) const {
  if (m_free) {
    fprintf(fp, "    <free>\n");
  } else {
    m_warp->print(fp);
    for (unsigned i = 0; i < MAX_REG_OPERANDS * 2; i++) {
      if (m_not_ready.test(i)) {
        std::string r = m_src_op[i].get_reg_string();
        fprintf(fp, "    '%s' not ready\n", r.c_str());
      }
    }
  }
}

void opndcoll_rfu_t::collector_unit_t::init(unsigned n, unsigned num_banks,
                                            unsigned log2_warp_size,
                                            const core_config *config,
                                            opndcoll_rfu_t *rfu,
                                            bool sub_core_model,
                                            unsigned banks_per_sched) {
  m_rfu = rfu;
  m_cuid = n;
  m_num_banks = num_banks;
  assert(m_warp == NULL);
  m_warp = new warp_inst_t(config);
  m_bank_warp_shift = log2_warp_size;
  m_sub_core_model = sub_core_model;
  m_num_banks_per_sched = banks_per_sched;
}

bool opndcoll_rfu_t::collector_unit_t::allocate(register_set *pipeline_reg_set,
                                                register_set *output_reg_set) {
  assert(m_free);
  assert(m_not_ready.none());
  m_free = false;
  m_output_register = output_reg_set;
  warp_inst_t **pipeline_reg = pipeline_reg_set->get_ready();
  if ((pipeline_reg) and !((*pipeline_reg)->empty())) {
    m_warp_id = (*pipeline_reg)->warp_id();
    for (unsigned op = 0; op < MAX_REG_OPERANDS; op++) {
      int reg_num =
          (*pipeline_reg)
              ->arch_reg.src[op];  // this math needs to match that used in
                                   // function_info::ptx_decode_inst
      if (reg_num >= 0) {          // valid register
        m_src_op[op] = op_t(this, op, reg_num, m_num_banks, m_bank_warp_shift,
                            m_sub_core_model, m_num_banks_per_sched,
                            (*pipeline_reg)->get_schd_id());
        m_not_ready.set(op);
      } else
        m_src_op[op] = op_t();
    }
    // move_warp(m_warp,*pipeline_reg);
    pipeline_reg_set->move_out_to(m_warp);
    return true;
  }
  return false;
}

void opndcoll_rfu_t::collector_unit_t::dispatch() {
  assert(m_not_ready.none());
  // move_warp(*m_output_register,m_warp);
  m_output_register->move_in(m_warp);
  m_free = true;
  m_output_register = NULL;
  for (unsigned i = 0; i < MAX_REG_OPERANDS * 2; i++) m_src_op[i].reset();
}

void exec_simt_core_cluster::create_shader_core_ctx() {
  m_core = new shader_core_ctx *[m_config->n_simt_cores_per_cluster];
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++) {
    unsigned sid = m_config->cid_to_sid(i, m_cluster_id);
    m_core[i] = new exec_shader_core_ctx(m_gpu, this, sid, m_cluster_id,
                                         m_config, m_mem_config, m_stats);
    m_core_sim_order.push_back(i);
  }
}

simt_core_cluster::simt_core_cluster(class gpgpu_sim *gpu, unsigned cluster_id,
                                     const shader_core_config *config,
                                     const memory_config *mem_config,
                                     shader_core_stats *stats,
                                     class memory_stats_t *mstats) {
  m_config = config;
  m_cta_issue_next_core = m_config->n_simt_cores_per_cluster -
                          1;  // this causes first launch to use hw cta 0
  m_cluster_id = cluster_id;
  m_gpu = gpu;
  m_stats = stats;
  m_memory_stats = mstats;
  m_mem_config = mem_config;
}

void simt_core_cluster::core_cycle() {
  for (std::list<unsigned>::iterator it = m_core_sim_order.begin();
       it != m_core_sim_order.end(); ++it) {
    m_core[*it]->cycle();
  }

  if (m_config->simt_core_sim_order == 1) {
    m_core_sim_order.splice(m_core_sim_order.end(), m_core_sim_order,
                            m_core_sim_order.begin());
  }
}

void simt_core_cluster::reinit() {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    m_core[i]->reinit(0, m_config->n_thread_per_shader, true);
}

unsigned simt_core_cluster::max_cta(const kernel_info_t &kernel) {
  return m_config->n_simt_cores_per_cluster * m_config->max_cta(kernel);
}

unsigned simt_core_cluster::get_not_completed() const {
  unsigned not_completed = 0;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    not_completed += m_core[i]->get_not_completed();
  return not_completed;
}

void simt_core_cluster::print_not_completed(FILE *fp) const {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++) {
    unsigned not_completed = m_core[i]->get_not_completed();
    unsigned sid = m_config->cid_to_sid(i, m_cluster_id);
    fprintf(fp, "%u(%u) ", sid, not_completed);
  }
}

float simt_core_cluster::get_current_occupancy(
    unsigned long long &active, unsigned long long &total) const {
  float aggregate = 0.f;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++) {
    aggregate += m_core[i]->get_current_occupancy(active, total);
  }
  return aggregate / m_config->n_simt_cores_per_cluster;
}

unsigned simt_core_cluster::get_n_active_cta() const {
  unsigned n = 0;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    n += m_core[i]->get_n_active_cta();
  return n;
}

unsigned simt_core_cluster::get_n_active_sms() const {
  unsigned n = 0;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    n += m_core[i]->isactive();
  return n;
}

unsigned simt_core_cluster::issue_block2core() {
  unsigned num_blocks_issued = 0;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++) {
    unsigned core =
        (i + m_cta_issue_next_core + 1) % m_config->n_simt_cores_per_cluster;

    kernel_info_t *kernel;
    // Jin: fetch kernel according to concurrent kernel setting
    if (m_config->gpgpu_concurrent_kernel_sm) {  // concurrent kernel on sm
      // always select latest issued kernel
      kernel_info_t *k = m_gpu->select_kernel();
      kernel = k;
    } else {
      // first select core kernel, if no more cta, get a new kernel
      // only when core completes
      kernel = m_core[core]->get_kernel();
      if (!m_gpu->kernel_more_cta_left(kernel)) {
        // wait till current kernel finishes
        if (m_core[core]->get_not_completed() == 0) {
          kernel_info_t *k = m_gpu->select_kernel();
          if (k) m_core[core]->set_kernel(k);
          kernel = k;
        }
      }
    }

    if (m_gpu->kernel_more_cta_left(kernel) &&
        //            (m_core[core]->get_n_active_cta() <
        //            m_config->max_cta(*kernel)) ) {
        m_core[core]->can_issue_1block(*kernel)) {
      m_core[core]->issue_block2core(*kernel);
      num_blocks_issued++;
      m_cta_issue_next_core = core;
      break;
    }
  }
  return num_blocks_issued;
}

void simt_core_cluster::cache_flush() {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    m_core[i]->cache_flush();
}

void simt_core_cluster::cache_invalidate() {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; i++)
    m_core[i]->cache_invalidate();
}

bool simt_core_cluster::icnt_injection_buffer_full(unsigned size, bool write) {
  unsigned request_size = size;
  if (!write) request_size = READ_PACKET_SIZE;
  return !::icnt_has_buffer(m_cluster_id, request_size);
}

void simt_core_cluster::icnt_inject_request_packet(class mem_fetch *mf) {
  // stats
  if (mf->get_is_write())
    m_stats->made_write_mfs++;
  else
    m_stats->made_read_mfs++;
  switch (mf->get_access_type()) {
    case CONST_ACC_R:
      m_stats->gpgpu_n_mem_const++;
      break;
    case TEXTURE_ACC_R:
      m_stats->gpgpu_n_mem_texture++;
      break;
    case GLOBAL_ACC_R:
      m_stats->gpgpu_n_mem_read_global++;
      break;
    // case GLOBAL_ACC_R: m_stats->gpgpu_n_mem_read_global++;
    // printf("read_global%d\n",m_stats->gpgpu_n_mem_read_global); break;
    case GLOBAL_ACC_W:
      m_stats->gpgpu_n_mem_write_global++;
      break;
    case LOCAL_ACC_R:
      m_stats->gpgpu_n_mem_read_local++;
      break;
    case LOCAL_ACC_W:
      m_stats->gpgpu_n_mem_write_local++;
      break;
    case INST_ACC_R:
      m_stats->gpgpu_n_mem_read_inst++;
      break;
    case L1_WRBK_ACC:
      m_stats->gpgpu_n_mem_write_global++;
      break;
    case L2_WRBK_ACC:
      m_stats->gpgpu_n_mem_l2_writeback++;
      break;
    case L1_WR_ALLOC_R:
      m_stats->gpgpu_n_mem_l1_write_allocate++;
      break;
    case L2_WR_ALLOC_R:
      m_stats->gpgpu_n_mem_l2_write_allocate++;
      break;
    default:
      assert(0);
  }

  // The packet size varies depending on the type of request:
  // - For write request and atomic request, the packet contains the data
  // - For read request (i.e. not write nor atomic), the packet only has control
  // metadata
  unsigned int packet_size = mf->size();
  if (!mf->get_is_write() && !mf->isatomic()) {
    packet_size = mf->get_ctrl_size();
  }
  m_stats->m_outgoing_traffic_stats->record_traffic(mf, packet_size);
  unsigned destination = mf->get_sub_partition_id();
  mf->set_status(IN_ICNT_TO_MEM,
                 m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
  if (!mf->get_is_write() && !mf->isatomic())
  {
    ::icnt_push(m_cluster_id, m_config->mem2device(destination), (void *)mf,
                mf->get_ctrl_size());
    if(mf && !mf->get_inst().empty())
    {
      if(print_stall_data)
        cout <<"ICNT_PUSH_TO_MEM_ENTER "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<cycles_passed<<" "<<going_from_shader_to_mem<<"\n";
      going_from_shader_to_mem++;
      SHADER_ICNT_PUSH++;
    }
  }
  else
  {
    ::icnt_push(m_cluster_id, m_config->mem2device(destination), (void *)mf,
                mf->size());
    if(mf && !mf->get_inst().empty())
    {
      if(print_stall_data)
        cout <<"ICNT_PUSH_TO_MEM_ENTER "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<cycles_passed<<" "<<going_from_shader_to_mem<<"\n";
      going_from_shader_to_mem++;
      SHADER_ICNT_PUSH++;
    }
  }
}

void simt_core_cluster::icnt_cycle() {
  if (!m_response_fifo.empty()) {
    mem_fetch *mf = m_response_fifo.front();
    unsigned cid = m_config->sid_to_cid(mf->get_sid());
    if (mf->get_access_type() == INST_ACC_R) {
      // instruction fetch response
      if (!m_core[cid]->fetch_unit_response_buffer_full()) {
        m_response_fifo.pop_front();
        m_core[cid]->accept_fetch_response(mf);
        CLUSTER_TO_SHADER_QUEUE_1_count = CLUSTER_TO_SHADER_QUEUE_1_count + 1;
        CLUSTER_TO_SHADER_QUEUE_1_cycle = CLUSTER_TO_SHADER_QUEUE_1_cycle + m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle - mf->get_status_change_cycle();
      }
    } else {
      // data response
      if (!m_core[cid]->ldst_unit_response_buffer_full()) {
        m_response_fifo.pop_front();
        CLUSTER_TO_SHADER_QUEUE_1_count = CLUSTER_TO_SHADER_QUEUE_1_count + 1;
        CLUSTER_TO_SHADER_QUEUE_1_cycle = CLUSTER_TO_SHADER_QUEUE_1_cycle + m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle - mf->get_status_change_cycle();
        m_memory_stats->memlatstat_read_done(mf);
        m_core[cid]->accept_ldst_unit_response(mf);
      }
    }
  }
  if (m_response_fifo.size() < m_config->n_simt_ejection_buffer_size) {
    mem_fetch *mf = (mem_fetch *)::icnt_pop(m_cluster_id);
    if(mf)
    {
      reached_shader_from_icnt = reached_shader_from_icnt + cycles_passed - mf->get_cycle_issued();
      mf->set_cycle_issued(cycles_passed);
    }
    if(mf && !mf->get_inst().empty())
    {
      //ICNT_TO_SHADER pop done here
      ICNT_TO_SHADER_count = ICNT_TO_SHADER_count + 1;
      ICNT_TO_SHADER_cycles = ICNT_TO_SHADER_cycles + m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle - mf->get_status_change_cycle();
      if(print_stall_data)
        cout <<"ICNT_PUSH_FROM_MEM_EXIT "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle - mf->get_status_change_cycle()<<" "<<going_from_shader_to_mem<<"\n";
    }
    if (!mf) return;
    assert(mf->get_tpc() == m_cluster_id);
    assert(mf->get_type() == READ_REPLY || mf->get_type() == WRITE_ACK);

    // The packet size varies depending on the type of request:
    // - For read request and atomic request, the packet contains the data
    // - For write-ack, the packet only has control metadata
    unsigned int packet_size =
        (mf->get_is_write()) ? mf->get_ctrl_size() : mf->size();
    m_stats->m_incoming_traffic_stats->record_traffic(mf, packet_size);
    mf->set_status(IN_CLUSTER_TO_SHADER_QUEUE,
                   m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
    if(mf->get_inst().space.get_type() == global_space && mf->get_inst().is_load() && print_stall_data)
    {
      cout << "PUSHING_GLOBAL_MEM_ICNT_TO_RESP "<<mf->get_inst().pc<<" "<<mf->get_inst().warp_id()<<" "<<mf->get_inst().get_sid()<<" "<<cycles_passed<<"\n";
    }
    // m_memory_stats->memlatstat_read_done(mf,m_shader_config->max_warps_per_shader);
    m_response_fifo.push_back(mf);
    m_stats->n_mem_to_simt[m_cluster_id] += mf->get_num_flits(false);
  }
}

void simt_core_cluster::get_pdom_stack_top_info(unsigned sid, unsigned tid,
                                                unsigned *pc,
                                                unsigned *rpc) const {
  unsigned cid = m_config->sid_to_cid(sid);
  m_core[cid]->get_pdom_stack_top_info(tid, pc, rpc);
}

void simt_core_cluster::display_pipeline(unsigned sid, FILE *fout,
                                         int print_mem, int mask) {
  m_core[m_config->sid_to_cid(sid)]->display_pipeline(fout, print_mem, mask);

  fprintf(fout, "\n");
  fprintf(fout, "Cluster %u pipeline state\n", m_cluster_id);
  fprintf(fout, "Response FIFO (occupancy = %zu):\n", m_response_fifo.size());
  for (std::list<mem_fetch *>::const_iterator i = m_response_fifo.begin();
       i != m_response_fifo.end(); i++) {
    const mem_fetch *mf = *i;
    mf->print(fout);
  }
}

void simt_core_cluster::print_cache_stats(FILE *fp, unsigned &dl1_accesses,
                                          unsigned &dl1_misses) const {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->print_cache_stats(fp, dl1_accesses, dl1_misses);
  }
}

void simt_core_cluster::get_icnt_stats(long &n_simt_to_mem,
                                       long &n_mem_to_simt) const {
  long simt_to_mem = 0;
  long mem_to_simt = 0;
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_icnt_power_stats(simt_to_mem, mem_to_simt);
  }
  n_simt_to_mem = simt_to_mem;
  n_mem_to_simt = mem_to_simt;
}

void simt_core_cluster::get_cache_stats(cache_stats &cs) const {
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_cache_stats(cs);
  }
}

void simt_core_cluster::get_L1I_sub_stats(struct cache_sub_stats &css) const {
  struct cache_sub_stats temp_css;
  struct cache_sub_stats total_css;
  temp_css.clear();
  total_css.clear();
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_L1I_sub_stats(temp_css);
    total_css += temp_css;
  }
  css = total_css;
}
void simt_core_cluster::get_L1D_sub_stats(struct cache_sub_stats &css) const {
  struct cache_sub_stats temp_css;
  struct cache_sub_stats total_css;
  temp_css.clear();
  total_css.clear();
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_L1D_sub_stats(temp_css);
    total_css += temp_css;
  }
  css = total_css;
}
void simt_core_cluster::get_L1C_sub_stats(struct cache_sub_stats &css) const {
  struct cache_sub_stats temp_css;
  struct cache_sub_stats total_css;
  temp_css.clear();
  total_css.clear();
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_L1C_sub_stats(temp_css);
    total_css += temp_css;
  }
  css = total_css;
}
void simt_core_cluster::get_L1T_sub_stats(struct cache_sub_stats &css) const {
  struct cache_sub_stats temp_css;
  struct cache_sub_stats total_css;
  temp_css.clear();
  total_css.clear();
  for (unsigned i = 0; i < m_config->n_simt_cores_per_cluster; ++i) {
    m_core[i]->get_L1T_sub_stats(temp_css);
    total_css += temp_css;
  }
  css = total_css;
}

void exec_shader_core_ctx::checkExecutionStatusAndUpdate(warp_inst_t &inst,
                                                         unsigned t,
                                                         unsigned tid) {
  if (inst.isatomic()) m_warp[inst.warp_id()]->inc_n_atomic();
  if (inst.space.is_local() && (inst.is_load() || inst.is_store())) {
    new_addr_type localaddrs[MAX_ACCESSES_PER_INSN_PER_THREAD];
    unsigned num_addrs;
    num_addrs = translate_local_memaddr(
        inst.get_addr(t), tid,
        m_config->n_simt_clusters * m_config->n_simt_cores_per_cluster,
        inst.data_size, (new_addr_type *)localaddrs);
    inst.set_addr(t, (new_addr_type *)localaddrs, num_addrs);
  }
  if (ptx_thread_done(tid)) {
    m_warp[inst.warp_id()]->set_completed(t);
    m_warp[inst.warp_id()]->ibuffer_flush();
    ibuffer_flush_count3++;
  }

  // PC-Histogram Update
  unsigned warp_id = inst.warp_id();
  unsigned pc = inst.pc;
  for (unsigned t = 0; t < m_config->warp_size; t++) {
    if (inst.active(t)) {
      int tid = warp_id * m_config->warp_size + t;
      cflog_update_thread_pc(m_sid, tid, pc);
    }
  }
}
