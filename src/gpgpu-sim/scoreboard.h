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

#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <vector>
#include "assert.h"

#ifndef SCOREBOARD_H_
#define SCOREBOARD_H_

#include "../abstract_hardware_model.h"

class Scoreboard {
 public:
  Scoreboard(unsigned sid, unsigned n_warps, class gpgpu_t *gpu);

  void reserveRegisters(const warp_inst_t *inst);
  void releaseRegisters(const warp_inst_t *inst);
  void releaseRegister(unsigned wid, unsigned regnum);

  bool checkCollision(unsigned wid, const inst_t *inst, unsigned SM) const;
  bool pendingWrites(unsigned wid) const;
  void printContents() const;
  const bool islongop(unsigned warp_id, unsigned regnum);

  void reserveRegistersMem(const warp_inst_t *inst);
  void releaseRegistersMem(const warp_inst_t *inst);
  void releaseRegisterMem(unsigned wid, unsigned regnum);

  void reserveRegistersComp(const warp_inst_t *inst);
  void releaseRegistersComp(const warp_inst_t *inst);
  void releaseRegisterComp(unsigned wid, unsigned regnum);

  //bool checkCollisionMem(unsigned wid, const inst_t *inst, unsigned SM) const;
  bool pendingWritesMem(unsigned wid) const;
  //bool checkCollisionComp(unsigned wid, const inst_t *inst, unsigned SM) const;
  bool pendingWritesComp(unsigned wid) const;

  std::vector<int> checkCollisionMem(unsigned wid, const inst_t *inst, unsigned SM) const;
  std::vector<int> checkCollisionComp(unsigned wid, const inst_t *inst, unsigned SM) const;

 private:
  void reserveRegister(unsigned wid, unsigned regnum);
  int get_sid() const { return m_sid; }

  unsigned m_sid;

  // keeps track of pending writes to registers
  // indexed by warp id, reg_id => pending write count
  std::vector<std::set<unsigned> > reg_table;
  // Data structure to store all the used registers in a warp
  std::vector<std::set<unsigned>> reg_used;
  // Data structure to store the last cycle in which these registers were released
  std::vector<std::vector<int>> reg_release_cycle;
  // Data structure to store the last cycle in which these registers were reserved 
  std::vector<std::vector<int>> reg_reserve_cycle;
  // Register that depend on a long operation (global, local or tex memory)
  std::vector<std::set<unsigned> > longopregs;

  void reserveRegisterMem(unsigned wid, unsigned regnum);
  void reserveRegisterComp(unsigned wid, unsigned regnum);

  // EACH WARP HAS ITS OWN SET OF REGISTERS --> HUNCH

  //keep track of pending writes to memory operations
  std::vector<std::set<unsigned> > reg_table_mem;
  //keep track of pending writes to computation operations
  std::vector<std::set<unsigned> > reg_table_comp;
  // Register that depend on a long operation (global, local or tex memory)
  // Register that depend on a long local mem operation (global, local or tex memory)
  std::vector<std::set<unsigned> > longopregs_local;
  // Register that depend on a long global mem operation (global, local or tex memory)
  std::vector<std::set<unsigned> > longopregs_global;
  // Register that depend on a long tex mem operation (global, local or tex memory)
  std::vector<std::set<unsigned> > longopregs_tex;

  // Data structure to store all the used registers in a warp
  std::vector<std::set<unsigned>> reg_used_mem;
  // Data structure to store the last cycle in which these registers were released
  std::vector<std::vector<int>> reg_release_cycle_mem;
  // Data structure to store the last cycle in which these registers were reserved 
  std::vector<std::vector<int>> reg_reserve_cycle_mem;

  // Data structure to store all the used registers in a warp
  std::vector<std::set<unsigned>> reg_used_comp;
  // Data structure to store the last cycle in which these registers were released
  std::vector<std::vector<int>> reg_release_cycle_comp;
  // Data structure to store the last cycle in which these registers were reserved 
  std::vector<std::vector<int>> reg_reserve_cycle_comp;

  class gpgpu_t *m_gpu;
};

#endif /* SCOREBOARD_H_ */
