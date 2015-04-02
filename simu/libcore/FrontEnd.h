#ifndef FRONT_END_H
#define FRONT_END_H

#include "nanassert.h"
#include "EmulInterface.h"
#include "BPred.h"
#include "GStats.h"
#include "FastQueue.h"
#include "pool.h"

class FrontEnd {
private:
	BPredictor *bpred;

	const int32_t FetchWidth;
	const uint32_t Delay;
	
	// [sizhuo] instQ here is considered as ALL front-end pipline stage + REAL "instQ"
	const int32_t maxInstQSize;
	int32_t freeInstQSize;
	FastQueue<DInst*> instQ;

	DInst *missBranch; // [sizhuo] mispredicted branch inst that cause stall
	bool flushing; // [sizhuo] in flushing mode, stop fetch & drain instQ

	typedef FastQueue<DInst*> InstBucket;
	FastQueue<InstBucket*> bucketPool;

	void enqInstQ(InstBucket *buck);
	typedef CallbackMember1<FrontEnd, InstBucket*, &FrontEnd::enqInstQ> enqInstQCB;

public:
	FrontEnd(FlowID id);
	~FrontEnd();
	void fetch(EmulInterface *eint, FlowID fid);
	void unblock(DInst *dinst);
	bool isBlocked() const {
		return missBranch != 0 || flushing;
	}
	DInst *firstInst() {
		return instQ.empty() ? 0 : instQ.top();
	}
	void deqInst() {
		I(!instQ.empty());
		instQ.pop();
		freeInstQSize++; // [sizhuo] inc free size
		I(freeInstQSize <= maxInstQSize);
	}
	bool isFlushing() { return flushing && missBranch == 0; }
	void startFlush() {
		flushing = true;
		missBranch = 0;
		// [sizhuo] freeInstQSize is recovered gradually
	}
	void endFlush() {
		I(flushing);
		I(freeInstQSize == maxInstQSize);
		I(missBranch == 0);
		flushing = false;
		// [sizhuo] redundant for safety
		freeInstQSize = maxInstQSize;
		missBranch = 0; 
	}
	bool empty() {
		return freeInstQSize == maxInstQSize;
	}
};

#endif
