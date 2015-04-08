#include "SescConf.h"

#include "WMMProcessor.h"
#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "TaskHandler.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "EmuSampler.h"
#include <stdlib.h>

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
  , avgFetchWidth("P(%d)_avgFetchWidth",i)
	, nKilled("P(%d)_killedInst", i)
	, nExcep("P(%d)_exception", i)
{
  bzero(RAT,sizeof(DInst*)*LREG_MAX);
}

WMMProcessor::~WMMProcessor() {}

void WMMProcessor::fetch(FlowID fid) {
  I(fid == cpu_id);
  I(active);
  I(eint);

  if(!frontEnd.isBlocked()) {
		frontEnd.fetch(eint, fid);
  }
}

void WMMProcessor::issueToROB() {
	int32_t nIssued = 0;
	while(nIssued < IssueWidth) {
		// [sizhou] peek first inst in instQ
		DInst *dinst = frontEnd.firstInst();
		if(dinst == 0) {
			// [sizhuo] instQ empty, done
			return;
		}
		// [sizhuo] try to issue the inst
		StallCause sc = addInst(dinst);
		if(sc != NoStall) {
			// [sizhuo] issue fail, change stat & done
			if(nIssued < RealisticWidth) {
				nStall[sc]->add(RealisticWidth - nIssued, dinst->getStatsFlag());
			}
			return;
		}
		// [sizhuo] issue success, deq inst & incr counter
		frontEnd.deqInst();
		nIssued++;
	}
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
		dinst->dump("WARNING: rename stage src2 dependency already set");
	}
	if( RAT[inst->getSrc1()] ) {
		RAT[inst->getSrc1()]->addSrc1(dinst);
	}
	if( RAT[inst->getSrc2()] ) {
		RAT[inst->getSrc2()]->addSrc2(dinst);
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
	// [sizhuo] stats
  if(!rob.empty()) {
    robUsed.sample(rob.size(), rob.front()->getStatsFlag());
	}

	// [sizhuo] retire from ROB
  for(uint16_t i=0 ; i<RetireWidth && !rob.empty() ; i++) {
    DInst *dinst = rob.front();
		I(dinst);

		// [sizhuo] check whether ID is increasing
		GIS(lastComID >= dinst->getID(), MSG("ERROR: retire ID OOO, min %lu, real %lu", lastComID, dinst->getID()));

    if (!dinst->isExecuted()) {
			return;
		}
    I(dinst->getCluster());
 
		// [sizhuo] detect replay inst
		// XXX: the replayed inst should not be dequeued from ROB
		if(replayRecover && dinst->getID() == replayID) {
			nExcep.inc(dinst->getStatsFlag()); // [sizhuo] stats
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
			// [sizhuo] stop here, flushing work is done in subsequent cycles
			return;
		}
    
		// [sizhuo] retire from func unit (2nd input has no real effect)
    bool done = dinst->getCluster()->retire(dinst, false);
		// [sizhuo] cannot retire, stop
    if( !done ) {
			return;
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
		nKilled.inc(dinst->getStatsFlag()); // [sizhuo] stats
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
		nKilled.inc(dinst->getStatsFlag()); // [sizhuo] stats
		dinst->destroy(eint);
		rob.pop_back();
		nRobDrained++; // [sizhuo] incr counter
	}
}

bool WMMProcessor::advance_clock(FlowID fid) {
	if(!active) {
		return false;
	}

	// [sizhuo] schedule deadlock check
	if(!lockCheckEnabled) {
		lockCheckEnabled = true;
		retire_lock_checkCB.scheduleAbs(globalClock + 100000);
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

	// [sizhuo] stats
	bool getStatsFlag = false; // [sizhuo] stats flag
  if( !rob.empty() ) {
    getStatsFlag = rob.front()->getStatsFlag();
  }
  clockTicks.inc(getStatsFlag);
  setWallClock(getStatsFlag);

	// [sizhuo] some black magic here...
  if (unlikely(throttlingRatio>1)) { 
    throttling_cntr++;

    uint32_t skip = (uint32_t)ceil(throttlingRatio/getTurboRatio()); 

    if (throttling_cntr < skip) {
      return true;
    }
    throttling_cntr = 1;
  }

	// [sizhuo] check active again
	if(!active) {
		return false;
	}

	// [sizhuo] in replay mode, no fetch or issue
	if(replayRecover) {
		if(flushing) {
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
	// [sizhuo] lock front-end (but doesn't drain inst)
	frontEnd.startFlush();
}

void WMMProcessor::retire_lock_check()
  // Detect simulator locks and flush the pipeline 
{
  RetireState state;
  if (active) {
    state.committed = nCommitted.getDouble();
		state.killed = nKilled.getDouble();
  }else{
    state.committed = 0;
		state.killed = 0;
  }
  if (!rob.empty()) {
    state.dinst    = rob.front();
    state.dinst_ID = rob.front()->getID();
  }

  if (last_state == state && active) {
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
