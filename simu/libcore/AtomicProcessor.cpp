#include "AtomicProcessor.h"

bool AtomicProcessor::inRoi = false;

AtomicProcessor::AtomicProcessor(GMemorySystem *gm, CPU_t i) 
	: GProcessor(gm, i, 1)
{
}

bool AtomicProcessor::advance_clock(FlowID fid) {
	if(!active) {
		return false;
	}

	// [sizhuo] black magic
	if (unlikely(throttlingRatio>1)) { 
		throttling_cntr++;

		uint32_t skip = (uint32_t)ceil(throttlingRatio/getTurboRatio()); 

		if (throttling_cntr < skip) {
			return true;
		}
		throttling_cntr = 1;
	}

	// [sizhuo] fast forwarding
	DInst *dinst = eint->executeHead(fid);
	if(dinst == 0) {
		return true;
	}
	const Instruction *const ins = dinst->getInst();
	// [sizhuo] check ROI begin/end
	if(ins->isRoiBegin()) {
		I(!inRoi);
		inRoi = true;
	} else if(ins->isRoiEnd()) {
		I(inRoi);
		inRoi = false;
	}
	// [sizhuo] dump ld & st
	if(inRoi) {
		if(ins->isLoad() || ins->isStore()) {
			dinst->dump("LdSt");
		}
		// [sizhuo] stats
		nCommitted.inc(dinst->getStatsFlag());
	}
	// [sizhuo] retire inst & return
	dinst->markIssued();
	dinst->markExecuted();
	dinst->destroy(eint);
	return true;
}
