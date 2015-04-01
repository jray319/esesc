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
	spaceInInstQueue = InstQueueSize;
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
	if((ROB.size() + rROB.size()) >= MaxROBSize) {
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
  if (sc != NoStall)
    return sc;
	// [sizhuo] now canIssue returns NoStall, this has incur side effects in func unit
	// we must issue this inst to both ROB & cluster

  const Instruction *inst = dinst->getInst();

	// [sizhuo] stats of inst type
  nInst[inst->getOpcode()]->inc(dinst->getStatsFlag());

	// [sizhuo] issue to ROB & cluster
	ROB.push(dinst);

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
	// [sizhuo] move executed inst from ROB to rROB
	while(!ROB.empty()) {
		DInst *dinst = ROB.top();

		if( !dinst->isExecuted() ) break;

		// [sizhuo] preretire from func unit, no flushing now
		bool done = dinst->getClusterResource()->preretire(dinst, false);
		if(!done) break;

		rROB.push(dinst);
		ROB.pop();
	}

	// [sizhuo] stats
  if(!ROB.empty())
    robUsed.sample(ROB.size(), ROB.top()->getStatsFlag());

  if(!rROB.empty())
    rrobUsed.sample(rROB.size(), rROB.top()->getStatsFlag());

	// [sizhuo] retire from rROB
  for(uint16_t i=0 ; i<RetireWidth && !rROB.empty() ; i++) {
    DInst *dinst = rROB.top();

    if (!dinst->isExecuted()) break;
    
    I(dinst->getCluster());
 
		// [sizhuo] retire from func unit, no flushing now
    bool done = dinst->getCluster()->retire(dinst, false);
		// [sizhuo] cannot retire, stop
    if( !done ) return;
    
		// [sizhuo] stats
		nCommitted.inc(dinst->getStatsFlag());

		// [sizhuo] truly retire inst
    dinst->destroy(eint);
		rROB.pop();
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
  if( !ROB.empty() ) {
    getStatsFlag = ROB.top()->getStatsFlag();
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
  if (!rROB.empty()) {
    state.r_dinst    = rROB.top();
    state.r_dinst_ID = rROB.top()->getID();
  }
  
  if (!ROB.empty()) {
    state.dinst    = ROB.top();
    state.dinst_ID = ROB.top()->getID();
  }

  if (last_state == state && active) {
    I(0);
    MSG("WARNING: Lock detected in P(%d)", getId());
    if (!rROB.empty()) {
			rROB.top()->dump("rROB top");
//      replay(rROB.top());
    }
    if (!ROB.empty()) {
			char str[100];
			sprintf(str, "Lock in P(%d), ROB top", getId());
			ROB.top()->dump(str);
//      ROB.top()->markExecuted();
//      replay(ROB.top());
    }
  }

  last_state = state;

  retire_lock_checkCB.scheduleAbs(globalClock + 100000);
}
