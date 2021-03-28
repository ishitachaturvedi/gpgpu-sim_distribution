// Copyright (c) 2009-2011, Tor M. Aamodt, Inderpreet Singh
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

#include "scoreboard.h"
#include "../cuda-sim/ptx_sim.h"
#include "shader.h"
#include "shader_trace.h"
#include <iostream>
#include "fast.h"

using namespace std;

// Constructor
Scoreboard::Scoreboard(unsigned sid, unsigned n_warps, class gpgpu_t* gpu)
    : longopregs() {
  m_sid = sid;
  // Initialize size of table
  reg_table.resize(n_warps);
  reg_used.resize(n_warps);
  reg_release_cycle.resize(n_warps);
  reg_reserve_cycle.resize(n_warps);
  reg_used_mem.resize(n_warps);
  reg_release_cycle_mem.resize(n_warps);
  reg_reserve_cycle_mem.resize(n_warps);
  reg_used_comp.resize(n_warps);
  reg_release_cycle_comp.resize(n_warps);
  reg_reserve_cycle_comp.resize(n_warps);
  longopregs.resize(n_warps);
  reg_table_mem.resize(n_warps);
  reg_table_comp.resize(n_warps); 
  longopregs_local.resize(n_warps);
  longopregs_global.resize(n_warps);
  longopregs_tex.resize(n_warps);
  m_gpu = gpu;
}

// Print scoreboard contents
void Scoreboard::printContents() const {
  printf("scoreboard contents (sid=%d): \n", m_sid);
  for (unsigned i = 0; i < reg_table.size(); i++) {
    if (reg_table[i].size() == 0) continue;
    printf("  wid = %2d: ", i);
    std::set<unsigned>::const_iterator it;
    for (it = reg_table[i].begin(); it != reg_table[i].end(); it++)
      printf("%u ", *it);
    printf("\n");
  }
}

void Scoreboard::reserveRegister(unsigned wid, unsigned regnum) {
  if (!(reg_table[wid].find(regnum) == reg_table[wid].end())) {
    printf(
        "Error: trying to reserve an already reserved register (sid=%d, "
        "wid=%d, regnum=%d).",
        m_sid, wid, regnum);
    abort();
  }
  SHADER_DPRINTF(SCOREBOARD, "Reserved Register - warp:%d, reg: %d\n", wid,
                 regnum);
  reg_table[wid].insert(regnum);

  // check if the regnum has never been added to reg_used
  if (!(reg_used[wid].find(regnum) != reg_used[wid].end()))
  {
    // The regnum has not been added to reg_used
    reg_used[wid].insert(regnum);
    auto finder = reg_used[wid].find(regnum);
    auto pos = std::distance(reg_used[wid].begin(), finder);
    reg_reserve_cycle[wid].insert(reg_reserve_cycle[wid].begin()+pos,cycle_counter);
    reg_release_cycle[wid].insert(reg_release_cycle[wid].begin()+pos,-1);
  }
  else
  {
    auto finder = reg_used[wid].find(regnum);
    auto pos = std::distance(reg_used[wid].begin(), finder);
    reg_reserve_cycle[wid][pos]=cycle_counter;
  }
}

void Scoreboard::reserveRegisterMem(unsigned wid, unsigned regnum) {
  reg_table_mem[wid].insert(regnum);

  if (!(reg_used_mem[wid].find(regnum) != reg_used_mem[wid].end()))
  {
    // The regnum has not been added to reg_used
    reg_used_mem[wid].insert(regnum);
    auto finder = reg_used_mem[wid].find(regnum);
    auto pos = std::distance(reg_used_mem[wid].begin(), finder);
    reg_reserve_cycle_mem[wid].insert(reg_reserve_cycle_mem[wid].begin()+pos,cycle_counter);
    reg_release_cycle_mem[wid].insert(reg_release_cycle_mem[wid].begin()+pos,-1);
  }
  else
  {
    auto finder = reg_used_mem[wid].find(regnum);
    auto pos = std::distance(reg_used_mem[wid].begin(), finder);
    reg_reserve_cycle_mem[wid][pos]=cycle_counter;
  }
}

void Scoreboard::reserveRegisterComp(unsigned wid, unsigned regnum) {
  reg_table_comp[wid].insert(regnum);

  if (!(reg_used_comp[wid].find(regnum) != reg_used_comp[wid].end()))
  {
    // The regnum has not been added to reg_used
    reg_used_comp[wid].insert(regnum);
    auto finder = reg_used_comp[wid].find(regnum);
    auto pos = std::distance(reg_used_comp[wid].begin(), finder);
    reg_reserve_cycle_comp[wid].insert(reg_reserve_cycle_comp[wid].begin()+pos,cycle_counter);
    reg_release_cycle_comp[wid].insert(reg_release_cycle_comp[wid].begin()+pos,-1);
  }
  else
  {
    auto finder = reg_used_comp[wid].find(regnum);
    auto pos = std::distance(reg_used_comp[wid].begin(), finder);
    reg_reserve_cycle_comp[wid][pos]=cycle_counter;
  }
}

// Unmark register as write-pending
void Scoreboard::releaseRegister(unsigned wid, unsigned regnum) {
  if (!(reg_table[wid].find(regnum) != reg_table[wid].end())) return;
  SHADER_DPRINTF(SCOREBOARD, "Release register - warp:%d, reg: %d\n", wid,
                 regnum);
  reg_table[wid].erase(regnum);
  {
    auto finder = reg_used[wid].find(regnum);
    auto pos = std::distance(reg_used[wid].begin(), finder);
    reg_release_cycle[wid][pos]=cycle_counter;
  }
}

//unmark registers as write Pending for mem operations
void Scoreboard::releaseRegisterMem(unsigned wid, unsigned regnum) {
  if (!(reg_table_mem[wid].find(regnum) != reg_table_mem[wid].end())) return;
  reg_table_mem[wid].erase(regnum);

  auto finder = reg_used_mem[wid].find(regnum);
  auto pos = std::distance(reg_used_mem[wid].begin(), finder);
  reg_release_cycle_mem[wid][pos]=cycle_counter;
}

//unmark registers as write pending for comp operations
void Scoreboard::releaseRegisterComp(unsigned wid, unsigned regnum) {
  if (!(reg_table_comp[wid].find(regnum) != reg_table_comp[wid].end())) return;
  reg_table_comp[wid].erase(regnum);

  auto finder = reg_used_comp[wid].find(regnum);
  auto pos = std::distance(reg_used_comp[wid].begin(), finder);
  reg_release_cycle_comp[wid][pos]=cycle_counter;
}

const bool Scoreboard::islongop(unsigned warp_id, unsigned regnum) {
  return longopregs[warp_id].find(regnum) != longopregs[warp_id].end();
}

void Scoreboard::reserveRegisters(const class warp_inst_t* inst) {
 //cout << "SB warp "<<inst->warp_id() << " reg ";
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    //DEBUG
    //cout << inst->out[r] << " ";
    if (inst->out[r] > 0) {
      reserveRegister(inst->warp_id(), inst->out[r]);
      SHADER_DPRINTF(SCOREBOARD, "Reserved register - warp:%d, reg: %d\n",
                     inst->warp_id(), inst->out[r]);
    }
  }
  //cout<<"\n";

  // Keep track of long operations
  if (inst->is_load() && (inst->space.get_type() == global_space ||
                          inst->space.get_type() == local_space ||
                          inst->space.get_type() == param_space_kernel ||
                          inst->space.get_type() == param_space_local ||
                          inst->space.get_type() == param_space_unclassified ||
                          inst->space.get_type() == tex_space)) {
    for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
      if (inst->out[r] > 0) {
        SHADER_DPRINTF(SCOREBOARD, "New longopreg marked - warp:%d, reg: %d\n",
                       inst->warp_id(), inst->out[r]);
        longopregs[inst->warp_id()].insert(inst->out[r]);
      }
    }
  }
}

void Scoreboard::reserveRegistersMem(const class warp_inst_t* inst) {
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    if (inst->out[r] > 0) {
      reserveRegisterMem(inst->warp_id(), inst->out[r]);
      SHADER_DPRINTF(SCOREBOARD, "Reserved register - warp:%d, reg: %d\n",
                     inst->warp_id(), inst->out[r]);
    }
  }

  // Keep track of long operations
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
	if (inst->out[r] > 0) {
         if (inst->is_load())
         {
	  if(inst->space.get_type() == global_space)
		  longopregs_global[inst->warp_id()].insert(inst->out[r]);
	  if(inst->space.get_type() == local_space)
		  longopregs_local[inst->warp_id()].insert(inst->out[r]);
	  if(inst->space.get_type() == tex_space)
		  longopregs_tex[inst->warp_id()].insert(inst->out[r]);
        }
    }
  }
}

void Scoreboard::reserveRegistersComp(const class warp_inst_t* inst) {
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    if (inst->out[r] > 0) {
      reserveRegisterComp(inst->warp_id(), inst->out[r]);
      SHADER_DPRINTF(SCOREBOARD, "Reserved register - warp:%d, reg: %d\n",
                     inst->warp_id(), inst->out[r]);
    }
  }

}

// Release registers for an instruction
void Scoreboard::releaseRegisters(const class warp_inst_t* inst) {
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    if (inst->out[r] > 0) {
      SHADER_DPRINTF(SCOREBOARD, "Register Released - warp:%d, reg: %d\n",
                     inst->warp_id(), inst->out[r]);
      releaseRegister(inst->warp_id(), inst->out[r]);
      longopregs[inst->warp_id()].erase(inst->out[r]);
    }
  }
}

// Release registers for a mem instruction
void Scoreboard::releaseRegistersMem(const class warp_inst_t* inst) {
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    if (inst->out[r] > 0) {
      releaseRegisterMem(inst->warp_id(), inst->out[r]);
      //longopregs_mem[inst->warp_id()].erase(inst->out[r]);
      if(inst->space.get_type() == global_space)
                  longopregs_global[inst->warp_id()].erase(inst->out[r]);
          if(inst->space.get_type() == local_space)
                  longopregs_local[inst->warp_id()].erase(inst->out[r]);
          if(inst->space.get_type() == tex_space)
                  longopregs_tex[inst->warp_id()].erase(inst->out[r]);
    }
  }
}

// Release registers for a comp instruction
void Scoreboard::releaseRegistersComp(const class warp_inst_t* inst) {
  for (unsigned r = 0; r < MAX_OUTPUT_VALUES; r++) {
    if (inst->out[r] > 0) {
      releaseRegisterComp(inst->warp_id(), inst->out[r]);
    }
  }
}

/**
 * Checks to see if registers used by an instruction are reserved in the
 *scoreboard
 *
 * @return
 * true if WAW or RAW hazard (no WAR since in-order issue)
 **/
bool Scoreboard::checkCollision(unsigned wid, const class inst_t* inst,unsigned SM) const {
  // Get list of all input and output registers
  std::set<int> inst_regs;

  for (unsigned iii = 0; iii < inst->outcount; iii++)
    inst_regs.insert(inst->out[iii]);

  for (unsigned jjj = 0; jjj < inst->incount; jjj++)
    inst_regs.insert(inst->in[jjj]);

  if (inst->pred > 0) inst_regs.insert(inst->pred);
  if (inst->ar1 > 0) inst_regs.insert(inst->ar1);
  if (inst->ar2 > 0) inst_regs.insert(inst->ar2);

  // Check for collision, get the intersection of reserved registers and
  // instruction registers
  std::set<int>::const_iterator it2;
  
  // check for longest taking release Reg and print the corresponding reserve and release cycle with the register number
  // unsigned reg_num = -1;
  // int reserve_c = -1;
  // int release_c = -1;
  // for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
  // {
  //   auto finder = reg_used[wid].find(*it2);
  //   if (reg_used[wid].find(*it2) != reg_used[wid].end())
  //   {
  //     auto pos = std::distance(reg_used[wid].begin(), finder);
  //     if (reg_release_cycle[wid][pos] > release_c)
  //     {
  //       reg_num = *it2;
  //       reserve_c = reg_reserve_cycle[wid][pos];
  //       release_c = reg_release_cycle[wid][pos];
  //     }
  //   }
  // }
  // cout <<"S "<<SM<<" W "<<wid<<" R "<< reg_num << " res "<< reserve_c <<" rel "<<release_c<<"\n";

  //SB conflict check
  for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
  {
    if (reg_table[wid].find(*it2) != reg_table[wid].end()) {
      return true;
    }
  }
  return false;
}

std::vector<int> Scoreboard::checkCollisionMem(unsigned wid, const class inst_t* inst, unsigned SM) const {
  // Get list of all input and output registers
  std::set<int> inst_regs;
  std::vector<int> result;
  result.resize(3);

  for (unsigned iii = 0; iii < inst->outcount; iii++)
    inst_regs.insert(inst->out[iii]);

  for (unsigned jjj = 0; jjj < inst->incount; jjj++)
    inst_regs.insert(inst->in[jjj]);

  if (inst->pred > 0) inst_regs.insert(inst->pred);
  if (inst->ar1 > 0) inst_regs.insert(inst->ar1);
  if (inst->ar2 > 0) inst_regs.insert(inst->ar2);

  // Check for collision, get the intersection of reserved registers and
  // instruction registers
  std::set<int>::const_iterator it2;

  // check for longest taking release Reg and print the corresponding reserve and release cycle with the register number
  int reg_num = -1;
  int reserve_c = -1;
  int release_c = -1;
  for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
  {
    auto finder = reg_used_mem[wid].find(*it2);
    if (reg_used_mem[wid].find(*it2) != reg_used_mem[wid].end())
    {
      auto pos = std::distance(reg_used_mem[wid].begin(), finder);
      //cout <<"bef "<<SM<<" W "<<wid<<" R "<< *it2 << " res "<< reg_reserve_cycle_mem[wid][pos] <<" rel "<<reg_release_cycle_mem[wid][pos]<<"\n";
      if (reg_release_cycle_mem[wid][pos] > release_c)
      {
        reg_num = *it2;
        reserve_c = reg_reserve_cycle_mem[wid][pos];
        release_c = reg_release_cycle_mem[wid][pos];
      }
    }
  }
  result[1] = reserve_c;
  result[2] = release_c;
  //cout <<"MS "<<SM<<" W "<<wid<<" R "<< reg_num << " res "<< reserve_c <<" rel "<<release_c<<"\n";
  //cout<<"************\n";

  //SB
  for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
    if (reg_table_mem[wid].find(*it2) != reg_table_mem[wid].end()) {
	    result[0] = 1;
      return result;
    }
  result[0] = 0;
  return result;
}

std::vector<int> Scoreboard::checkCollisionComp(unsigned wid, const class inst_t* inst, unsigned SM) const {
  // Get list of all input and output registers
  std::set<int> inst_regs;
  std::vector<int> result;
  result.resize(3);

  for (unsigned iii = 0; iii < inst->outcount; iii++)
    inst_regs.insert(inst->out[iii]);

  for (unsigned jjj = 0; jjj < inst->incount; jjj++)
    inst_regs.insert(inst->in[jjj]);

  if (inst->pred > 0) inst_regs.insert(inst->pred);
  if (inst->ar1 > 0) inst_regs.insert(inst->ar1);
  if (inst->ar2 > 0) inst_regs.insert(inst->ar2);

  // Check for collision, get the intersection of reserved registers and
  // instruction registers
  std::set<int>::const_iterator it2;

  // check for longest taking release Reg and print the corresponding reserve and release cycle with the register number
  int reg_num = -1;
  int reserve_c = -1;
  int release_c = -1;
  for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
  {
    auto finder = reg_used_comp[wid].find(*it2);
    if (reg_used_comp[wid].find(*it2) != reg_used_comp[wid].end())
    {
      auto pos = std::distance(reg_used_comp[wid].begin(), finder);
      //cout <<"bef "<<SM<<" W "<<wid<<" R "<< *it2 << " res "<< reg_reserve_cycle_comp[wid][pos] <<" rel "<<reg_release_cycle_comp[wid][pos]<<"\n";
      if (reg_release_cycle_comp[wid][pos] > release_c)
      {
        reg_num = *it2;
        reserve_c = reg_reserve_cycle_comp[wid][pos];
        release_c = reg_release_cycle_comp[wid][pos];
      }
    }
  }

  result[1] = reserve_c;
  result[2] = release_c;
  //cout <<"CS "<<SM<<" W "<<wid<<" R "<< reg_num << " res "<< reserve_c <<" rel "<<release_c<<"\n";
  //cout<<"************\n";
  
  //SB
  for (it2 = inst_regs.begin(); it2 != inst_regs.end(); it2++)
    if (reg_table_comp[wid].find(*it2) != reg_table_comp[wid].end()) {
      result[0] = 1;
      return result;
    }
  result[0] = 0;
  return result;
}

bool Scoreboard::pendingWrites(unsigned wid) const {
  return !reg_table[wid].empty();
}

bool Scoreboard::pendingWritesMem(unsigned wid) const {
  return !reg_table_mem[wid].empty();
}

bool Scoreboard::pendingWritesComp(unsigned wid) const {
  return !reg_table_comp[wid].empty();
}
