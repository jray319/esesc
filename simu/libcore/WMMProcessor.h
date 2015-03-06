#ifndef _WMMPROCESSOR_H_
#define _WMMPROCESSOR_H_

#include "nanassert.h"
#include "GProcessor.h"

// [sizhuo] Processor for WMM
class WMMProcessor : public GProcessor {
private:
	LSQNone lsq; // load/store queue: currently just a dummy one
  void fetch(FlowID fid);

protected:
	bool advance_clock(FlowID fid);
	StallCause addInst(DInst *dinst);
	void retire();

public:
	WMMProcessor(GMemorySystem *gm, CPU_t i);
	virtual ~WMMProcessor();

	LSQ *getLSQ() { return &lsq; }
	void replay(DInst *target);
	bool isFlushing();
	bool isReplayRecovering();
	Time_t getReplayID();
};

#endif /* _WMMPROCESSOR_H_ */
