// Contributed by Jose Renau
//                Milos Prvulovic
//
// The ESESC/BSD License
//
// Copyright (c) 2005-2013, Regents of the University of California and 
// the ESESC Project.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   - Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
//
//   - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
//   - Neither the name of the University of California, Santa Cruz nor the
//   names of its contributors may be used to endorse or promote products
//   derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "Cluster.h"
#include "Resource.h"
#include "SescConf.h"
#include "ClusterScheduler.h"
/* }}} */
//#define DUMP_TRACE 1

ClusterScheduler::ClusterScheduler(const ResourcesPoolType ores) 
  : res(ores) {
}

ClusterScheduler::~ClusterScheduler() {
  
}

RoundRobinClusterScheduler::RoundRobinClusterScheduler(const ResourcesPoolType ores)
  : ClusterScheduler(ores) {
  
  nres.resize(res.size());
  pos.resize(res.size(),0);

  for(size_t i=0;i<res.size();i++) {
    nres[i] = res[i].size();
		// [sizhuo] Processor class assign resource only once
		// this may be inefficient, so we want to 1 opcode only has 1 resource
		GI(i > 0 && i < iMAX, nres[i] == 1);
  }
}

RoundRobinClusterScheduler::~RoundRobinClusterScheduler() {
  
}

Resource *RoundRobinClusterScheduler::getResource(DInst *dinst) {
  const Instruction *inst=dinst->getInst();
  InstOpcode type = inst->getOpcode();

  unsigned int i = pos[type];
  if (i>=nres[type]) {
    i = 0;
    pos[type] = 1;
  }else{
    pos[type]++;
  }

  I(i<res[type].size());

  return res[type][i];
}

LRUClusterScheduler::LRUClusterScheduler(const ResourcesPoolType ores)
  : ClusterScheduler(ores) {
  
}

LRUClusterScheduler::~LRUClusterScheduler() {
  
}

Resource *LRUClusterScheduler::getResource(DInst *dinst) {
  const Instruction *inst=dinst->getInst();
  InstOpcode type = inst->getOpcode();
  Resource *touse = res[type][0];
  
  for(size_t i = 1; i<res[type].size(); i++) {
    if (touse->getUsedTime() > res[type][i]->getUsedTime())
      touse = res[type][i];
  }
  
  touse->setUsedTime(); 
  return touse;
}
