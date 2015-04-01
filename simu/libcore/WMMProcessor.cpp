#include "SescConf.h"

#include "WMMProcessor.h"

#include "TaskHandler.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "EmuSampler.h"

WMMProcessor::WMMProcessor(GMemorySystem *gm, CPU_t i)
	: GProcessor(gm, i, 1)
  , frontEnd(i)
	, lsq(i)
  , retire_lock_checkCB(this)
  , clusterManager(gm, this)
  , avgFetchWidth("P(%d)_avgFetchWidth",i)
{
  bzero(RAT,sizeof(DInst*)*LREG_MAX);
	lockCheckEnabled = false;
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

	// [sizhuo] stats of inst type
  nInst[inst->getOpcode()]->inc(dinst->getStatsFlag());

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

void WMMProcessor::retire() {
	// [sizhuo] stats
  if(!rob.empty()) {
    robUsed.sample(rob.size(), rob.front()->getStatsFlag());
	}

	// [sizhuo] retire from ROB
  for(uint16_t i=0 ; i<RetireWidth && !rob.empty() ; i++) {
    DInst *dinst = rob.front();

    if (!dinst->isExecuted()) {
			return;
		}
    I(dinst->getCluster());
 
		// [sizhuo] retire from func unit (2nd input has no real effect)
    bool done = dinst->getCluster()->retire(dinst, false);
		// [sizhuo] cannot retire, stop
    if( !done ) {
			return;
		}
    
		// [sizhuo] stats
		nCommitted.inc(dinst->getStatsFlag());

		// [sizhuo] truly retire inst
    dinst->destroy(eint);
		rob.pop_front();
	}
}

bool WMMProcessor::advance_clock(FlowID fid) {
	if(!active) {
		return false;
	}

	// fetch
  fetch(fid);

	// [sizhuo] schedule deadlock check
	if(!lockCheckEnabled) {
		lockCheckEnabled = true;
		retire_lock_checkCB.scheduleAbs(globalClock + 100000);
	}

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

	// [sizhuo] issue inst into ROB
	issueToROB();

	// [sizhuo] retire
	retire();

	return true;
}

void WMMProcessor::replay(DInst *target) {
}

bool WMMProcessor::isFlushing() {
	return false;
}

bool WMMProcessor::isReplayRecovering() {
	return false;
}

Time_t WMMProcessor::getReplayID() {
	return 0;
}

void WMMProcessor::retire_lock_check()
  // Detect simulator locks and flush the pipeline 
{
  RetireState state;
  if (active) {
    state.committed = nCommitted.getDouble();
  }else{
    state.committed = 0;
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
