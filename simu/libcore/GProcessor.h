// Contributed by Jose Renau
//                Karin Strauss
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

#ifndef GPROCESSOR_H
#define GPROCESSOR_H

#define SCOORE_CORE 1//0

#include "estl.h"

#include <stdint.h>

// Generic Processor Interface.
//
// This class is a generic interface for Processors. It has been
// design for Traditional and SMT processors in mind. That's the
// reason why it manages the execution engine (RDEX).

#include "nanassert.h"

#include "callback.h"
#include "Cluster.h"
#include "ClusterManager.h"
#include "Instruction.h"
#include "FastQueue.h"
#include "GStats.h"
#include "Pipeline.h"
#include "EmulInterface.h"
#include "EmuSampler.h"


#include "Resource.h"
#include "Snippets.h"
#include "LSQ.h"


class FetchEngine;
class GMemorySystem;
class BPredictor;

class GProcessor {
  private:
  protected:
    // Per instance data
    const uint32_t cpu_id;
    const FlowID   MaxFlows;

    const int32_t FetchWidth;
    const int32_t IssueWidth;
    const int32_t RetireWidth;
    const int32_t RealisticWidth;
    const int32_t InstQueueSize;
    const size_t  MaxROBSize;

    EmulInterface   *eint; // [sizhuo] ifc to emulator
    GMemorySystem   *memorySystem; // [sizhuo] memory system 

    StoreSet           storeset; // [sizhuo] store set
		// [sizhuo] rROB contains inst able to retire but not guaranteed to retire
		// rROB is introduced mostly for simulation convenience
		// so there is no bandwidth limit from ROB to rROB
    FastQueue<DInst *> rROB; // ready/retiring/executed ROB
    FastQueue<DInst *> ROB; // [sizhuo] rob of inst not ready to retire

    // Updated by Processor or SMTProcessor. Shows the number of clocks
    // that the processor have been active (fetch + exe engine)
    ID(int32_t prevDInstID);

    bool       active;

    // BEGIN  Statistics
    //
    GStatsCntr *nStall[MaxStall];
    GStatsCntr *nInst[iMAX];

    // OoO Stats
    GStatsAvg  rrobUsed;
    GStatsAvg  robUsed;
    GStatsAvg  nReplayInst;
    GStatsCntr nCommitted; // committed instructions

    // "Lack of Retirement" Stats
    GStatsCntr noFetch;
    GStatsCntr noFetch2;

    GStatsCntr   nFreeze;
    GStatsCntr   clockTicks;

    static Time_t       lastWallClock;
    static GStatsCntr   *wallClock;

    // END Statistics
    float        throttlingRatio;
    uint32_t     throttling_cntr;

    uint64_t     lastReplay;

    // Construction
	// [sizhuo] stats for each type of uOP
    void buildInstStats(GStatsCntr *i[iMAX], const char *txt);
	// [sizhuo] build proc components (func unit -> cluseter -> all clustes)
    void buildUnit(const char *clusterName, GMemorySystem *ms, Cluster *cluster, InstOpcode type);
    void buildCluster(const char *clusterName, GMemorySystem * ms);
    void buildClusters(GMemorySystem *ms);

    GProcessor(GMemorySystem *gm, CPU_t i, size_t numFlows);
	// [sizhuo] issue to ROB from front-end, return number of issued uOPs
    int32_t issue(PipeQueue &pipeQ); 

    virtual void retire();
    virtual StallCause addInst(DInst *dinst) = 0; // [sizhuo] add inst to ROB & cluster

    virtual void fetch(FlowID fid) = 0;
  public:

    virtual ~GProcessor();
    int getId() const { return cpu_id; }
    GStatsCntr *getnCommitted() { return &nCommitted;}

    GMemorySystem *getMemorySystem() const { return memorySystem; }
    virtual LSQ *getLSQ() = 0;    
    virtual bool isFlushing() = 0;
    virtual bool isReplayRecovering() = 0;
    virtual Time_t getReplayID() = 0;

    // Notify the fetch that an exception/replay happen. Stall the rename until
    // the rob replay is retired.
    virtual void replay(DInst *target) { };// = 0;

    bool isROBEmpty() const { return ROB.empty() && rROB.empty(); }

    // Returns the maximum number of flows this processor can support
    FlowID getMaxFlows(void) const { return MaxFlows; }

    void report(const char *str);

    // Different types of cores extend this function. See SMTProcessor and
    // Processor.
    virtual bool advance_clock(FlowID fid) = 0;

    void setEmulInterface(EmulInterface *e) {
      eint = e;
    }

    void freeze(Time_t nCycles) {
      nFreeze.add(nCycles);
      clockTicks.add(nCycles);
    }

    void setActive() {
      active = true;
    }
    void clearActive() {
      active = false;
    }

    void setWallClock(bool en=true) {
      if (lastWallClock == globalClock || !en)
        return;
      lastWallClock = globalClock;
      wallClock->inc(en);
    }

    StoreSet *getSS() { return &storeset; }

#ifdef ENABLE_CUDA
    float getTurboRatioGPU() { return EmuSampler::getTurboRatioGPU(); };
#endif
    float getTurboRatio() { return EmuSampler::getTurboRatio(); };

};

#endif   // GPROCESSOR_H
