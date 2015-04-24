#ifndef ATOMIC_PROCESSOR_H
#define ATOMIC_PROCESSOR_H

#include "nanassert.h"

#include "GProcessor.h"
#include "Pipeline.h"
#include "FetchEngine.h"
#include "LSQ.h"

class AtomicProcessor : public GProcessor {
private:
	static bool inRoi;

protected:
	virtual void fetch(FlowID fid) {}
	virtual void retire() {}
	virtual StallCause addInst(DInst *dinst) { return NoStall; }

public:
	AtomicProcessor(GMemorySystem *gm, CPU_t i);
	virtual ~AtomicProcessor() {}
	virtual LSQ *getLSQ() { return 0; }
	virtual bool isFlushing() { return false; }
	virtual bool isReplayRecovering() { return false; }
	virtual Time_t getReplayID() { return DInst::invalidID; }
	virtual bool advance_clock(FlowID fid);
};

#endif
