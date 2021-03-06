#include "SescConf.h"

#include "WMMProcessor.h"
#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "TaskHandler.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "EmuSampler.h"
#include <stdlib.h>

WMMProcessor::SimStage WMMProcessor::simStage = WMMProcessor::Forward;
Time_t WMMProcessor::simBeginTime = 0;
GStatsCntr* WMMProcessor::simTime = 0;

WMMProcessor::WMMProcessor(GMemorySystem *gm, CPU_t i)
  : GProcessor(gm, i, 1)
  , frontEnd(i)
  , replayRecover(false)
  , replayID(0)
  , flushing(0)
#ifdef DEBUG
  , lastReplayValid(0)
  , lastReplayPC(0)
  , lastComID(DInst::invalidID)
  , startExcep(false)
  , excepDelay(1000)
  , genExcepCB(this)
#endif
  , lockCheckEnabled(false)
  , retire_lock_checkCB(this)
  , clusterManager(gm, this)
  , nActiveCyc("P(%d)_activeCyc", i)
  , robUsage("P(%d)_robUsage", i)
  , ldQUsage("P(%d)_ldQUsage", i)
  , exLdNum("P(%d)_exLdNum", i)
  , doneLdNum("P(%d)_doneLdNum", i)
  , stQUsage("P(%d)_stQUsage", i)
  , comSQUsage("P(%d)_comSQUsage", i)
  , retireStallByEmpty("P(%d)_retireStallByEmpty", i)
  , retireStallByComSQ("P(%d)_retireStallByComSQ", i)
{
  // [sizhuo] issue/fetch/retire width must be equal in WMM
  if(IssueWidth != FetchWidth || IssueWidth != RetireWidth) {
    SescConf->notCorrect();
  }

  // [sizhuo] clear rename table
  bzero(RAT,sizeof(DInst*)*LREG_MAX);

  // [sizhuo] simTim counter
  if(simTime == 0) {
    simTime = new GStatsCntr("OS:simTime");
    I(simTime);
  }

  // [sizhuo] stats counters
  for(int t = 0; t < DInst::MaxReason; t++) {
    nExcep[t] = new GStatsCntr("P(%d)_nExcepBy_%s", cpu_id, DInst::replayReason2String(static_cast<DInst::ReplayReason>(t)));
    nKilled[t] = new GStatsCntr("P(%d)_nKilledBy_%s", cpu_id, DInst::replayReason2String(static_cast<DInst::ReplayReason>(t)));
    retireStallByFlush[t] = new GStatsCntr("P(%d)_retireStallByFlush_%s", cpu_id, DInst::replayReason2String(static_cast<DInst::ReplayReason>(t)));
    retireStallByVerify[t] = new GStatsCntr("P(%d)_retireStallByVerify_%s", cpu_id, DInst::replayReason2String(static_cast<DInst::ReplayReason>(t)));
    I(nExcep[t]);
    I(nKilled[t]);
    I(retireStallByFlush[t]);
    I(retireStallByVerify[t]);
  }

  for(int t = 0; t < iMAX; t++) {
    retireStallByEx[t] = new GStatsCntr("P(%d)_retireStallByEx_%s", cpu_id, Instruction::opcode2Name(static_cast<InstOpcode>(t)));
    I(retireStallByEx[t]);
  }

  for(int t = 0; t < MaxStall; t++) {
    issueStall[t] = 0;
  }
  issueStall[SmallWinStall] = new GStatsCntr("P(%d)_issueStallByOutWin",i);
  issueStall[SmallROBStall] = new GStatsCntr("P(%d)_issueStallByOutROB",i);
  issueStall[SmallREGStall] = new GStatsCntr("P(%d)_issueStallByOutReg",i);
  issueStall[OutsLoadsStall] = new GStatsCntr("P(%d)_issueStallByOutLd",i);
  issueStall[OutsStoresStall] = new GStatsCntr("P(%d)_issueStallByOutSt",i);
  issueStall[OutsBranchesStall] = new GStatsCntr("P(%d)_issueStallByOutBr",i);
  issueStall[ReplaysStall] = new GStatsCntr("P(%d)_issueStallByReplay",i);
  issueStall[SyscallStall] = new GStatsCntr("P(%d)_issueStallBySyscall",i);
  issueStall[NoInstStall] = new GStatsCntr("P(%d)_issueStallByNoInst",i);
  for(int t = 1; t < MaxStall; t++) {
    // [sizhuo] 0 is NoStall, never should be used
    I(issueStall[t]);
  }

  for(int t = 0; t < DInst::MaxReason; t++) {
    issueStallByReplay[t] = new GStatsCntr("P(%d)_issueStallByReplay_%s", cpu_id, DInst::replayReason2String(static_cast<DInst::ReplayReason>(t)));
    I(issueStallByReplay[t]);
  }
}

WMMProcessor::~WMMProcessor() {}

void WMMProcessor::fetch(FlowID fid) {
  I(fid == cpu_id);
  I(eint);

  if(!active) {
    return;
  }

  if(!frontEnd.isBlocked()) {
    frontEnd.fetch(eint, fid);
  }
}

void WMMProcessor::issueToROB() {
  int32_t n2Issue = IssueWidth;
  for(; n2Issue > 0; n2Issue--) {
    // [sizhou] peek first inst in instQ
    DInst *dinst = frontEnd.firstInst();
    if(dinst == 0) {
      // [sizhuo] instQ empty, done & incr counter for NoInstStall
      issueStall[NoInstStall]->add(n2Issue, true);
      return;
    }
    // [sizhuo] XXX: we are in no-sampling mode, every inst should have stats flag on
    if(!dinst->getStatsFlag()) {
      I(0);
      MSG("WARNING: P(%d) fetch inst %lu has stats flag off", cpu_id, dinst->getID());
    }
    // [sizhuo] try to issue the inst
    StallCause sc = addInst(dinst);
    if(sc != NoStall) {
      // [sizhuo] issue fail, change stat & done
      issueStall[sc]->add(n2Issue, dinst->getStatsFlag());
      // [sizhuo] for small win stall, track to the cluster
      if(sc == SmallWinStall) {
        (dinst->getCluster()->smallWinIssueStall).add(n2Issue, dinst->getStatsFlag());
      }
      return;
    }
    // [sizhuo] issue success, deq inst & incr counter
    frontEnd.deqInst();
  }
  I(n2Issue == 0);
}

StallCause WMMProcessor::addInst(DInst *dinst) {
  // [sizhuo] rob size limit
  if(rob.size() >= MaxROBSize) {
    return SmallROBStall;
  }

  // [sizhuo] get the cluster to ex this inst
  Cluster *cluster = dinst->getCluster();
  if( !cluster ) {
    Resource *res = clusterManager.getResource(dinst);
    cluster       = res->getCluster();
    dinst->setCluster(cluster, res);
  }

  // [sizhuo] whether we can add dinst into cluster
  StallCause sc = cluster->canIssue(dinst);
  if (sc != NoStall) {
    return sc;
  }
  // [sizhuo] now canIssue returns NoStall, this has incur side effects in func unit
  // we must issue this inst to both ROB & cluster

  const Instruction *inst = dinst->getInst();

  // [sizhuo] issue to ROB & cluster
  rob.push_back(dinst);

  // [sizhuo] dependency of src regs of dinst
  I(RAT[0] == 0); // [sizhuo] read R0 should not cause dependency
  if( !dinst->isSrc2Ready() ) {
    // [sizhuo] copied from OooProcessor.cpp
    // here is some black magic I can't understand...
    // when we add the dependency of src2??
    dinst->dump("WARNING: rename stage src2 dependency already set");
    if( RAT[inst->getSrc1()] ) {
      RAT[inst->getSrc1()]->addSrc1(dinst);
    }
  } else {
    if( RAT[inst->getSrc1()] ) {
      RAT[inst->getSrc1()]->addSrc1(dinst);
    }
    if( RAT[inst->getSrc2()] ) {
      RAT[inst->getSrc2()]->addSrc2(dinst);
    }
  }
  dinst->setRAT1Entry(&RAT[inst->getDst1()]);
  dinst->setRAT2Entry(&RAT[inst->getDst2()]);

  // [sizhuo] issue to cluster
  I(dinst->getCluster() != 0); // Resource::schedule must set the resource field
  dinst->getCluster()->addInst(dinst);

  // [sizhuo] write rename table
  RAT[inst->getDst1()] = dinst;
  RAT[inst->getDst2()] = dinst;

  I(dinst->getCluster());

  return NoStall;
}

void WMMProcessor::retireFromROB(FlowID fid) {
  // [sizhuo] retire from ROB
  int32_t n2Retire = RetireWidth;
  for(; n2Retire > 0 && !rob.empty() ; n2Retire--) {
    DInst *dinst = rob.front();
    I(dinst);

    // [sizhuo] check whether ID is increasing
    GIS(lastComID >= dinst->getID(), MSG("ERROR: retire ID OOO, min %lu, real %lu", lastComID, dinst->getID()));

    if (!dinst->isExecuted()) {
      // [sizhuo] stall by un-executed inst
      retireStallByEx[dinst->getInst()->getOpcode()]->add(n2Retire, dinst->getStatsFlag());
      // [sizhuo] use the remaining retire BW to retire stores
      for(; n2Retire > 0 ; n2Retire--) {
        bool success = mtLSQ->retireStEarly();
        if(!success) {
          return;
        }
      }
      return;
    }
    I(dinst->getCluster());
 
    // [sizhuo] detect replay inst
    // XXX: the replayed inst should not be dequeued from ROB
    if(replayRecover && dinst->getID() == replayID) {
      I(replayReason < DInst::MaxReason);
      nExcep[replayReason]->inc(dinst->getStatsFlag()); // [sizhuo] stats
      // [sizhuo] start flushing
      I(!flushing);
      flushing = true;
      // [sizhuo] poison all inst in ROB
      for(std::deque<DInst*>::iterator iter = rob.begin(); iter != rob.end(); iter++) {
        I(*iter);
        (*iter)->markPoisoned();
      }
      // [sizhuo] reset all components
      reset();
      // [sizhuo] sync head & tail in emulator
      // XXX: after fixing the ID of the return inst of DInst::clone()
      // DInst::ID won't become out-of-order
      eint->syncHeadTail(fid);
#ifdef DEBUG
      // [sizhuo] record info about replay inst for debug
      lastReplayValid = true;
      lastReplayPC = dinst->getPC();
      lastReplayInst.copy(dinst->getInst());
#endif
      // [sizhuo] retire stall by flushing
      I(replayReason < DInst::MaxReason);
      retireStallByFlush[replayReason]->add(n2Retire, dinst->getStatsFlag());
      // [sizhuo] stop here, flushing work is done in subsequent cycles
      return;
    }
    
    // [sizhuo] retire from func unit (2nd input has no real effect)
    bool done = dinst->getCluster()->retire(dinst, false);
    // [sizhuo] cannot retire, stop
    if( !done ) {
      // [sizhuo] stall by commit SQ non-empty OR load verification
      if(dinst->getInst()->isComFence()) {
        // [sizhuo] stall by commit fence in TSO/WMM
        I(mtLSQ->getComSQUsage() > 0);
        retireStallByComSQ.add(n2Retire, dinst->getStatsFlag());
      } else if(dinst->getInst()->isLoad()) {
        if(dinst->getReplayReason() != DInst::MaxReason) {
          // [sizhuo] stall by verification load in SC/TSO
          I(dinst->getReplayReason() == DInst::CacheRep || dinst->getReplayReason() == DInst::CacheInv);
          retireStallByVerify[dinst->getReplayReason()]->add(n2Retire, dinst->getStatsFlag());
        } else {
          // [sizhuo] stall by pending stores in SC
          I(mtLSQ->getComSQUsage() > 0);
          retireStallByComSQ.add(n2Retire, dinst->getStatsFlag());
        }
      } else {
        I(0);
      }
      // [sizhuo] use the remaining retire BW to retire stores
      for(; n2Retire > 0 ; n2Retire--) {
        bool success = mtLSQ->retireStEarly();
        if(!success) {
          return;
        }
      }
      return;
    }

    // [sizhuo] check ROI end (only in sim stage)
    if(simStage == Sim && dinst->getInst()->isRoiEnd()) {
      // [sizhuo] switch to Done stage
      simStage = Done;
      // [sizhuo] set sim time counter
      I(simTime->getSamples() == 0);
      I(globalClock > simBeginTime);
      simTime->add(globalClock - simBeginTime, true);
      MSG("INFO: simulation ends @ %lu", globalClock);
    }

    // [sizhuo] stats
    nCommitted.inc(dinst->getStatsFlag());
    nInst[dinst->getInst()->getOpcode()]->inc(dinst->getStatsFlag());

    // [sizhuo] next commit ID min value
    ID(lastComID = dinst->getID());

    // [sizhuo] truly retire inst
    dinst->destroy(eint);
    rob.pop_front();
  }

  if(rob.empty() && n2Retire > 0) {
    // [sizhuo] stall by ROB empty
    retireStallByEmpty.add(n2Retire, true);
  }
}

void WMMProcessor::doFlush() {
  I(replayRecover);
  I(flushing);
  // [sizhuo] check whether flushing is done
  if(rob.empty() && frontEnd.empty()) {
    // [sizhuo] flushing is done, return to normal state
    frontEnd.endFlush();
    replayID = 0;
    replayRecover = false;
    flushing = false;
    replayReason = DInst::MaxReason;
    I(isReset());
    // [sizhuo] for testing, generate exception
    //ID(genExcepCB.schedule(excepDelay));

    return;
  }

  // [sizhuo] flush not done
  // XXX: we can't destroy any inst before emulator re-sync head & tail

  // [sizhuo] drain front-end (limited by issue width)
  I(frontEnd.isFlushing());
  int32_t nFrontDrained = 0;
  while(nFrontDrained < IssueWidth) {
    DInst *dinst = frontEnd.firstInst();
    if(dinst == 0) {
      break;
    }
    I(replayReason < DInst::MaxReason);
    nKilled[replayReason]->inc(dinst->getStatsFlag()); // [sizhuo] stats
    frontEnd.deqInst();
    nFrontDrained++; // [sizhuo] incr counter
    // [sizhuo] destroy the inst
    dinst->markIssued();
    dinst->markExecuted();
    dinst->destroy(eint);
  }

  // [sizhuo] drain ROB from tail (limited by retire width)
  int32_t nRobDrained = 0;
  while(nRobDrained < RetireWidth && !rob.empty()) {
    DInst *dinst = rob.back();
    I(dinst);
    I(dinst->isPoisoned());
    // [sizhuo] non-executed inst cannot be drained
    if(!dinst->isExecuted()) {
      break;
    }
    // [sizhuo] dinst executed, drain it
    I(replayReason < DInst::MaxReason);
    nKilled[replayReason]->inc(dinst->getStatsFlag()); // [sizhuo] stats
    dinst->destroy(eint);
    rob.pop_back();
    nRobDrained++; // [sizhuo] incr counter
  }
}

bool WMMProcessor::advance_clock(FlowID fid) {
  if(!active) {
    if(frontEnd.empty() && rob.empty() && !replayRecover) {
      return false;
    } else {
      I(0);
      MSG("WARNING: P(%d) inactive but frontEnd.empty = %d, rob.empty = %d, replayRecover = %d", cpu_id, frontEnd.empty(), rob.empty(), replayRecover);
    }
  } else {
    // [sizhuo] some black magic here...
    if (unlikely(throttlingRatio>1)) { 
      throttling_cntr++;

      uint32_t skip = (uint32_t)ceil(throttlingRatio/getTurboRatio()); 

      if (throttling_cntr < skip) {
        return true;
      }
      throttling_cntr = 1;
    }
  }

  // [sizhuo] fast forwarding
  if(simStage == Forward) {
    I(eint);
    I(frontEnd.empty());
    I(rob.empty());
    I(!replayRecover);
    DInst *dinst = eint->executeHead(fid);
    if(dinst == 0) {
      return true;
    }
    // [sizhuo] check whether inst is ROI begin
    if(dinst->getInst()->isRoiBegin()) {
      // [sizhuo] change stage & set sim begin time
      simStage = Sim;
      simBeginTime = globalClock;
      MSG("INFO: simulation begins @ %lu", globalClock);
    }
    // [sizhuo] retire inst & return
    dinst->markIssued();
    dinst->markExecuted();
    dinst->destroy(eint);
    return true;
  }

  // [sizhuo] simulation done, we need to gracefully deal with remaining inst
  if(simStage == Done) {
    // [sizhuo] in replay mode
    if(replayRecover) {
      if(flushing) {
        // [sizhuo] do flush work
        doFlush();
      } else {
        // [sizhuo] not in flushing mode, just commit good inst
        retireFromROB(fid);
      }
      return true;
    }
    // [sizhuo] rob not empty, retire inst
    if(!rob.empty()) {
      retireFromROB(fid);
      return true;
    }
    // [sizhuo] front end not empty, just drain inst
    if(!frontEnd.empty()) {
      while(frontEnd.firstInst()) {
        DInst *dinst = frontEnd.firstInst();
        frontEnd.deqInst();
        dinst->markIssued();
        dinst->markExecuted();
        dinst->destroy(eint);
      }
      return true;
    }
    // [sizhuo] now whole pipeline empty, we can directly retire inst
    DInst *dinst = eint->executeHead(fid);
    if(dinst == 0) {
      return true;
    }
    dinst->markIssued();
    dinst->markExecuted();
    dinst->destroy(eint);
    return true;
  }

  // [sizhuo] schedule deadlock check
  if(active) {
    // [sizhuo] only do following stuff when proc is active
    if(!lockCheckEnabled) {
      lockCheckEnabled = true;
      retire_lock_checkCB.scheduleAbs(globalClock + 100000);
    }
  }

#ifdef DEBUG
  /*
  // [sizhuo] schedule exception test
  if(!startExcep) {
    startExcep = true;
    genExcepCB.schedule(excepDelay);
  }
  */
  // [sizhuo] check whether the first fetched inst after replay is the replayed inst
  if(lastReplayValid && !replayRecover) {
    DInst *dinst = frontEnd.firstInst();
    if(dinst) {
      if(*(dinst->getInst()) == lastReplayInst && dinst->getPC() == lastReplayPC) {
        lastReplayValid = false;
      } else {
        I(0);
        MSG("ERROR: P(%d) first fetched inst after replay is not the replayed inst", getId());
      }
    }
  }
#endif

  // [sizhuo] set wall clock 
  // this is the amount of time when at least 1 proc is not idling
  bool incrClk = !frontEnd.empty() || !rob.empty() || replayRecover;
  clockTicks.inc(incrClk);
  setWallClock(incrClk);

  // [sizhuo] incr active cycle count
  nActiveCyc.inc(true);

  // [sizhuo] sample ROB & LSQ sizes
  robUsage.sample(true, rob.size());
  ldQUsage.sample(true, mtLSQ->getLdQUsage());
  exLdNum.sample(true, mtLSQ->getExLdNum());
  doneLdNum.sample(true, mtLSQ->getDoneLdNum());
  stQUsage.sample(true, mtLSQ->getStQUsage());
  comSQUsage.sample(true, mtLSQ->getComSQUsage());

  // [sizhuo] in replay mode, no fetch or issue
  if(replayRecover) {
    // [sizhuo] issue is stalled by replay
    issueStall[ReplaysStall]->add(IssueWidth, true);
    I(replayReason < DInst::MaxReason);
    issueStallByReplay[replayReason]->add(IssueWidth, true);
    // [sizhuo] do replay work
    if(flushing) {
      // [sizhuo] retire stall by flush
      I(replayReason < DInst::MaxReason);
      retireStallByFlush[replayReason]->add(RetireWidth, true);
      // [sizhuo] do flush work
      doFlush();
    } else {
      // [sizhuo] not in flushing mode, just commit good inst
      retireFromROB(fid);
    }
    return true;
  }

  // [sizhuo] normal state
  fetch(fid); // [sizhuo] fetch
  issueToROB(); // [sizhuo] issue inst into ROB
  retireFromROB(fid); // [sizhuo] retire

  return true;
}

void WMMProcessor::reset() {
  // [sizhuo] reset rename table
  bzero(RAT,sizeof(DInst*)*LREG_MAX);
  // [sizhuo] reset LSQ
  mtLSQ->reset();
  // [sizhuo] reset store set
  mtStoreSet->reset();
  // [sizhou] reset clusters & resources
  clusterManager.reset();
}

bool WMMProcessor::isReset() {
  for(int i = 0; i < LREG_MAX; i++) {
    if(RAT[i] != 0) {
      I(0);
      return false;
    }
  }
  return mtLSQ->isReset() && mtStoreSet->isReset() && clusterManager.isReset();
}

void WMMProcessor::replay(DInst *target) {
  I(target);
  I(!target->isPoisoned());

  if(replayRecover && replayID < target->getID()) {
    // [sizhuo] target is younger than current replay inst, ignore it
    return;
  }
  // [sizhuo] flush must have not started
  I(!flushing);
  // [sizhuo] record replay inst
  replayID = target->getID();
  replayRecover = true;
  replayReason = target->getReplayReason();
  I(replayReason != DInst::MaxReason);
  // [sizhuo] lock front-end (but doesn't drain inst)
  frontEnd.startFlush();
}

void WMMProcessor::retire_lock_check()
  // Detect simulator locks and flush the pipeline 
{
  RetireState state;
  if (active) {
    state.committed = nCommitted.getDouble();
    state.killed = 0;
    for(int i = 0; i < DInst::MaxReason; i++) {
      state.killed += nKilled[i]->getDouble();
    }
  }else{
    state.committed = 0;
    state.killed = 0;
  }
  if (!rob.empty()) {
    state.dinst    = rob.front();
    state.dinst_ID = rob.front()->getID();
  }

  // [sizhuo] don't check deadlock when processor is idling
  bool procIdle = frontEnd.empty() && rob.empty() && !replayRecover;
  if (last_state == state && active && !procIdle) {
    I(0);
    MSG("WARNING: Lock detected in P(%d)", getId());
    if (!rob.empty()) {
      char str[100];
      sprintf(str, "Lock in P(%d), ROB front", getId());
      rob.front()->dump(str);
    }
  }

  last_state = state;

  retire_lock_checkCB.scheduleAbs(globalClock + 100000);
}

#ifdef DEBUG
void WMMProcessor::genExcep() {
  /*
  if(!rob.empty()) {
    // [sizhuo] ROB has inst, randomly select one in ROB to gen exception
    int32_t pos = rand() % rob.size();
    std::deque<DInst*>::iterator iter = rob.begin();
    while(pos > 0) {
      I(iter != rob.end());
      iter++;
      pos--;
    }
    I(iter != rob.end());
    I(*iter);
    replay(*iter);
  } else {
    // [sizhuo] ROB empty, try next time
    genExcepCB.schedule(excepDelay);
  }
  */
}
#endif

bool WMMProcessor::allOlderCtrlDone(Time_t id) {
  // [sizhuo] since this func is called for retire store early, id should not be the head of ROB
  I(rob.begin() != rob.end());
  I((*(rob.begin()))->getID() < id);
  // [sizhuo] we search from the head of the ROB
  for(std::deque<DInst*>::iterator iter = rob.begin(); iter != rob.end(); iter++) {
    DInst *dinst = *iter;
    I(dinst);
    // [sizhuo] reach the store, so all older branch are done
    if(dinst->getID() >= id) {
      return true;
    }
    // [sizhuo] check for poison (should not happen)
    if(dinst->isPoisoned()) {
      I(0);
      return false;
    }
    // [sizhuo] check for control
    if(dinst->getInst()->isControl()) {
      // [sizhuo] find unexecuted control inst
      if(!(dinst->isExecuted())) {
        return false;
      }
    }
  }
  // [sizhuo] should not reach here
  I(0);
  return false;
}
