#include "SescConf.h"

#include "WMMProcessor.h"

#include "TaskHandler.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "EmuSampler.h"

WMMProcessor::WMMProcessor(GMemorySystem *gm, CPU_t i)
	: GProcessor(gm, i, 1)
	, lsq(i)
{
}

WMMProcessor::~WMMProcessor() {}

void WMMProcessor::fetch(FlowID fid) {
}

StallCause WMMProcessor::addInst(DInst *dinst) {
	return NoStall;
}

void WMMProcessor::retire() {
}

bool WMMProcessor::advance_clock(FlowID fid) {
	if(!active) {
		return false;
	}

	I(eint);
	I(fid == cpu_id);

	// fetch
	DInst *dinst = eint->executeHead(fid);
	if(dinst == 0) {
		return false;
	}

	// stats
	bool getStatsFlag = dinst->getStatsFlag(); // [sizhuo] stats flag

  clockTicks.inc(getStatsFlag);
  setWallClock(getStatsFlag);
  nInst[dinst->getInst()->getOpcode()]->inc(getStatsFlag);
	nCommitted.inc(getStatsFlag);

	// retire
	dinst->markIssued();
	dinst->markExecuted();
	dinst->destroy(eint);

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
