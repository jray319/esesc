#include "SescConf.h"

#include "WMMProcessor.h"

#include "TaskHandler.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "EmuSampler.h"

WMMProcessor::WMMProcessor(GMemorySystem *gm, CPU_t i)
	: GProcessor(gm, i, 1)
  , IFID(i, this, gm)
  , pipeQ(i)
	, lsq(i)
  , clusterManager(gm, this)
  , avgFetchWidth("P(%d)_avgFetchWidth",i)
{
  bzero(RAT,sizeof(DInst*)*LREG_MAX);
	spaceInInstQueue = InstQueueSize;
	busy = false;
}

WMMProcessor::~WMMProcessor() {}

void WMMProcessor::fetch(FlowID fid) {
	// [sizhuo] copied from OoOProcessor.cpp
	
  I(fid == cpu_id);
  I(active);
  I(eint);

  if( IFID.isBlocked(0)) {
//    I(0);
    busy = true;
  }else{
    IBucket *bucket = pipeQ.pipeLine.newItem();
    if( bucket ) {
      IFID.fetch(bucket, eint, fid);
      if (!bucket->empty()) {
        avgFetchWidth.sample(bucket->size());
        busy = true;
      }
    }
  }
}

StallCause WMMProcessor::addInst(DInst *dinst) {
	// [sizhuo] blocking atomic core, only accept new inst when ROB & rROB are empty
	if(!ROB.empty() || !rROB.empty()) {
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
	// we issue this inst to both ROB & cluster

  const Instruction *inst = dinst->getInst();

	// [sizhuo] stats of inst type
  nInst[inst->getOpcode()]->inc(dinst->getStatsFlag());

	// [sizhuo] issue to ROB & cluster, no need to track dependency
	ROB.push(dinst);

  I(dinst->getCluster() != 0); // Resource::schedule must set the resource field
  dinst->getCluster()->addInst(dinst);

	// [sizhuo] write rename table
  dinst->setRAT1Entry(&RAT[inst->getDst1()]);
  dinst->setRAT2Entry(&RAT[inst->getDst2()]);
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
    
		//GI(!flushing, dinst->isExecuted());
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
	if(!busy) return false;

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

  // ID Stage (insert to instQueue)
  if( spaceInInstQueue >= FetchWidth ) {
    IBucket *bucket = pipeQ.pipeLine.nextItem();
    if( bucket ) {
      I(!bucket->empty());

      spaceInInstQueue -= bucket->size();
      pipeQ.instQueue.push(bucket);
    }else{
      noFetch2.inc(getStatsFlag);
    }
  }else{
    noFetch.inc(getStatsFlag);
  }

	// [sizhuo] issue inst into ROB
  if( !pipeQ.instQueue.empty() ) {
    spaceInInstQueue += issue(pipeQ);
  }else if( ROB.empty() && rROB.empty() ) {
    // Still busy if we have some in-flight requests
    busy = pipeQ.pipeLine.hasOutstandingItems();
    return true;
  }

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
