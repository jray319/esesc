// Contributed by Jose Renau
//                Milos Prvulovic
//                Smruti Sarangi
//                Luis Ceze
//                Kerry Veenstra
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

#ifndef FETCHENGINE_H
#define FETCHENGINE_H

#include "nanassert.h"

#include "EmulInterface.h"

#include "BPred.h"
#include "GStats.h"

class GMemorySystem;
class IBucket;

// A FetchEngine holds only one execution flow. An SMT processor
// should instantiate several FetchEngines.

class GProcessor;

// [sizhuo] fetch inst. 
// XXX: wrong path inst is not simulated, the delay is simulated
class FetchEngine {
private:

  GMemorySystem *const gms;
  GProcessor    *const gproc;

  BPredictor   *bpred; // [sizhuo] branch predictor

  uint16_t      FetchWidth; // [sizhuo] fetch bandwidth

  uint16_t      BB4Cycle; // [sizhuo] max number of basic blocks fetched in 1 cycle
  uint16_t      bpredDelay;
  uint16_t      maxBB;

  TimeDelta_t   BTACDelay;
  TimeDelta_t   IL1HitDelay; // [sizhuo] L1 hit delay, reset to 0 if we simulate L1 I$

  // InstID of the address that generated a misprediction
 
  uint32_t  numSP; // [sizhuo] unkown.. =1 for CPU processor in current configuration
  // [sizhuo] missInst, lastd, cbPending are all array of size numSP
  bool      *missInst; // branch missprediction. Stop fetching until solved
  DInst     **lastd; // [sizhuo] the branch inst that is mispredicted
  CallbackContainer *cbPending; // [sizhuo] all pending callbacks about fetch

  AddrType  missInstPC;

  bool      enableICache;
 
protected:
  //bool processBranch(DInst *dinst, uint16_t n2Fetched);
  // [sizhuo] when fetch a branch inst, we make prediction 
  // and determines whether we need to stall fetching 
  bool processBranch(DInst *dinst, uint16_t n2Fetchedi, uint16_t* maxbb);

  // ******************* Statistics section
  GStatsAvg  avgBranchTime;
  GStatsCntr nDelayInst1;
  GStatsCntr nDelayInst2;
  GStatsCntr nDelayInst3;
  GStatsCntr nFetched;
  GStatsCntr nBTAC;
  // *******************

public:
  FetchEngine(FlowID i
              ,GProcessor *gproc
              ,GMemorySystem *gms
              ,FetchEngine *fe = 0);

  ~FetchEngine();

  // [sizhuo] fetch inst, called by processor classes
  void fetch(IBucket *buffer, EmulInterface *eint, FlowID fid);
  typedef CallbackMember3<FetchEngine, IBucket *, EmulInterface* , FlowID, &FetchEngine::fetch>  fetchCB;

  // [sizhuo] do real work of fetch. oldinst -- the inst we failed to fetch last time, we retry to fetch it this time
  // n2Fetched -- remaining fetch bandwidth
  void realfetch(IBucket *buffer, EmulInterface *eint, FlowID fid, DInst* oldinst, int32_t n2Fetched, uint16_t maxbb);
  typedef CallbackMember6<FetchEngine, IBucket *, EmulInterface* , FlowID, DInst*, int32_t, uint16_t, &FetchEngine::realfetch>  realfetchCB;

  // [sizhuo] callback to perform all pending fetches, just this???
  void unBlockFetch(DInst* dinst, Time_t missFetchTime);
  typedef CallbackMember2<FetchEngine, DInst*, Time_t,  &FetchEngine::unBlockFetch> unBlockFetchCB;

  void unBlockFetchBPredDelay(DInst* dinst, Time_t missFetchTime);
  typedef CallbackMember2<FetchEngine, DInst* , Time_t, &FetchEngine::unBlockFetchBPredDelay> unBlockFetchBPredDelayCB;

#if 0
  void unBlockFetch();
  StaticCallbackMember0<FetchEngine,&FetchEngine::unBlockFetch> unBlockFetchCB;

  void unBlockFetchBPredDelay();
  StaticCallbackMember0<FetchEngine,&FetchEngine::unBlockFetchBPredDelay> unBlockFetchBPredDelayCB;
#endif

  void dump(const char *str) const;


  bool isBlocked(DInst* inst) const {
    for (uint32_t i = 0; i < numSP; i++){
      if (missInst[i] == false)
        return false;
    }
    return true;
  }

  void clearMissInst(DInst * dinst);
  void setMissInst(DInst * dinst);
    /*
  void clearMissInst(DInst * dinst) {
    MSG("UnLocking Dinst ID: %llu,DInst PE:%d, Dinst PC %x\n",dinst->getID(),dinst->getPE(),dinst->getPC());
    missInst[dinst->getPE()] = false;
  }

  void setMissInst(DInst * dinst) {
    MSG("CPU: %d\tLocking Dinst ID: %llu, DInst PE:%d, Dinst PC %x",(int) this->gproc->getId(), dinst->getID(),dinst->getPE(),dinst->getPC());
    I(dinst->getPE()!=0);
    I(!missInst[dinst->getPE()]);
    missInst[dinst->getPE()] = true;
    lastd = dinst;
  }
*/
};

#endif   // FETCHENGINE_H
