//  Contributed by Jose Renau
//                 Basilio Fraguela
//                 James Tuck
//                 Milos Prvulovic
//                 Luis Ceze
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

#include "SescConf.h"

#include "TaskHandler.h"
#include "InOrderProcessor.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "ClusterManager.h"

InOrderProcessor::InOrderProcessor(GMemorySystem *gm, CPU_t i)
  :GProcessor(gm, i, 1)
  ,IFID(i, this, gm)
  ,pipeQ(i)
  ,lsq(i)
  ,rROB(SescConf->getInt("cpusimu", "robSize", i))
  ,clusterManager(gm, this)
{
  spaceInInstQueue = InstQueueSize;
  bzero(RAT,sizeof(DInst*)*LREG_MAX);

  busy = false;
}

InOrderProcessor::~InOrderProcessor() {
  // Nothing to do
}

void InOrderProcessor::fetch(FlowID fid) {
  // TODO: Move this to GProcessor (same as in OoOProcessor)
  I(eint);
  I(active);

  if( IFID.isBlocked(0)) { // [sizhuo] can't fetch
    busy = true;
  }else{ // [sizhuo] fetch new inst from emulator
    IBucket *bucket = pipeQ.pipeLine.newItem();
    if( bucket ) { // [sizhuo] allocate new fetch req success
      IFID.fetch(bucket, eint, fid);
      if (!bucket->empty()) {
        busy = true;
      }
    }
  }
}

bool InOrderProcessor::advance_clock(FlowID fid) {

  if (!active) {
    // time to remove from the running queue
    //TaskHandler::removeFromRunning(cpu_id);
    return false;
  }
 
  fetch(fid); // [sizhuo] fetch stage: try to fetch new inst

  if (!busy) // [sizhuo] no work to do, just return
    return false;

  bool getStatsFlag = false;
  if( !ROB.empty() ) {
    getStatsFlag = ROB.top()->getStatsFlag();
  }
  clockTicks.inc(getStatsFlag);
  setWallClock(getStatsFlag);

  // ID Stage (insert to instQueue)
  if (spaceInInstQueue >= FetchWidth) {
    IBucket *bucket = pipeQ.pipeLine.nextItem();
    if( bucket ) {
      I(!bucket->empty());
      //      I(bucket->top()->getInst()->getAddr());

      spaceInInstQueue -= bucket->size();
      pipeQ.instQueue.push(bucket);
    }else{
      noFetch2.inc(getStatsFlag);
    }
  }else{
    noFetch.inc(getStatsFlag);
  }

  // RENAME Stage
  if ( !pipeQ.instQueue.empty() ) {
    spaceInInstQueue += issue(pipeQ);
  }else if (ROB.empty() && rROB.empty()) {
    // Still busy if we have some in-flight requests
    busy = pipeQ.pipeLine.hasOutstandingItems();
    return true;
  }

  // [sizhuo] the timing of dec & rename are both simulated in pipeQ.pipeline
  //
  // [sizhuo] inst execution has two types
  // 1. inst not subject to dependency are scheduled for execution in cluster.addInst()
  // 2. inst subject to dependency are scheduled for execution when waked up by others

  retire(); // [sizhuo] retire inst

  return true;
}

StallCause InOrderProcessor::addInst(DInst *dinst) {

  const Instruction *inst = dinst->getInst();

  // [sizhuo] if there is RAW/WAW dependency, we don't issue this uOP
  // FIXME: uOP without dst reg will write RAT[LREG_InvalidOutput], later uOP will stall on WAW hazard
  if(RAT[inst->getSrc1()] != 0 ||
     RAT[inst->getSrc2()] != 0 ||
     RAT[inst->getDst1()] != 0 ||
     RAT[inst->getDst2()] != 0) {
    return SmallWinStall;
  }

  // [sizhuo] we issue uOP to ROB & cluster at same time, so check both can issue or not
  if( (ROB.size()+rROB.size()) >= MaxROBSize )
    return SmallROBStall;

  Cluster *cluster = dinst->getCluster();
  if( !cluster ) { // [sizhuo] assign cluster to uOP
    Resource *res = clusterManager.getResource(dinst);
    cluster       = res->getCluster();
    dinst->setCluster(cluster, res);
  }

  StallCause sc = cluster->canIssue(dinst);
  if (sc != NoStall)
    return sc;

  // FIXME: rafactor the rest of the function that it is the same as in OoOProcessor (share same function in GPRocessor)

  // BEGIN INSERTION (note that cluster already inserted in the window)
  // dinst->dump("");

  nInst[inst->getOpcode()]->inc(dinst->getStatsFlag()); // FIXME: move to cluster

  // [sizhuo] after check, we can now truly issue the uOP
  ROB.push(dinst);

  // [sizhuo] seems to track dependency, but due to the early dependency check 
  // no dependency is added
  if( !dinst->isSrc2Ready() ) {
    // It already has a src2 dep. It means that it is solved at
    // retirement (Memory consistency. coherence issues)
    if( RAT[inst->getSrc1()] ) // [sizhuo] this will always be false
      RAT[inst->getSrc1()]->addSrc1(dinst);
  }else{
    if( RAT[inst->getSrc1()] ) // [sizhuo] this will always be false
      RAT[inst->getSrc1()]->addSrc1(dinst);

    if( RAT[inst->getSrc2()] ) // [sizhuo] this will always be false
      RAT[inst->getSrc2()]->addSrc2(dinst);
  }

  // [sizhuo] update rename table / scoreboard
  dinst->setRAT1Entry(&RAT[inst->getDst1()]);
  dinst->setRAT2Entry(&RAT[inst->getDst2()]);

  dinst->getCluster()->addInst(dinst);

  // [sizhuo] FIXME: should not update RAT if uOP is not writing dst reg
  RAT[inst->getDst1()] = dinst;
  RAT[inst->getDst2()] = dinst;

  I(dinst->getCluster());

  return NoStall;
}

void InOrderProcessor::retire() {

  // Pass all the ready instructions to the rrob
  bool stats = false;
  while(!ROB.empty()) {
	// [sizhuo] move executed inst from ROB to rROB
    DInst *dinst = ROB.top();
    stats = dinst->getStatsFlag();

    if( !dinst->isExecuted() )
      break; 

	// [sizhuo] actions in function unit
    bool done = dinst->getClusterResource()->preretire(dinst, false);
    if( !done )
      break;

    rROB.push(dinst);
    ROB.pop();

    nCommitted.inc(dinst->getStatsFlag());
  }

  robUsed.sample(ROB.size(), stats);
  rrobUsed.sample(rROB.size(), stats);

  // [sizhuo] retire from rROB, limit by retire bandwidth
  for(uint16_t i=0 ; i<RetireWidth && !rROB.empty() ; i++) {
    DInst *dinst = rROB.top();

    if (!dinst->isExecuted())
      break;

    I(dinst->getCluster());

    bool done = dinst->getCluster()->retire(dinst, false);
    if( !done ) {
      //dinst->getInst()->dump("not ret");
      return;
    }

#if 0
    FlowID fid = dinst->getFlowId();
    if( active) {
      EmulInterface *eint = TaskHandler::getEmul(fid);
      eint->reexecuteTail( fid );
    }
#endif

    dinst->destroy(eint);
    rROB.pop();
  }

}

void InOrderProcessor::replay(DInst *dinst) {

  MSG("Inorder cores do not support replays. Set MemoryReplay = false in the confguration");

  // FIXME: foo should be equal to the number of in-flight instructions (check OoOProcessor)
  size_t foo= 1;
  nReplayInst.sample(foo, dinst->getStatsFlag());

  // FIXME: How do we manage a replay in this processor??
}
