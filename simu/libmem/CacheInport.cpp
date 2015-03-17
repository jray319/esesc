#include "CacheInport.h"
#include <algorithm>

void FIFOCacheInport::doFirstMsg() {
	// [sizhuo] msgQ cannot be empty
	I(!msgQ.empty());
	if(msgQ.empty()) {
		MSG("WARNING: ACache inport try to handle msg when msgQ is empty");
		return;
	}
	// [sizhuo] get the first msg and call handler
	StaticCallbackBase *cb = msgQ.front();
	cb->call();
}

void FIFOCacheInport::enqMsgQ(StaticCallbackBase *cb) {
	bool initEmpty = msgQ.empty();
	msgQ.push(cb); // [sizhuo] enq
	// [sizhuo] if msgQ is initially empty, we must try to process this new msg
	if(initEmpty)	{
		cb->call();
	}
}

void FIFOCacheInport::enqNewMsg(StaticCallbackBase *cb, bool statsFlag) {
	// [sizhuo] contend for enq port
	Time_t when = enqPort->nextSlot(statsFlag); 
	I(when >= globalClock);
	// [sizhuo] scheule enq
	enqMsgQCB::scheduleAbs(when, this, cb);
}

void FIFOCacheInport::deqDoneMsg() {
	I(!msgQ.empty());
	if(msgQ.empty()) {
		MSG("WARNING: ACache inport try to pop msg when msgQ is empty");
		return;
	}
	msgQ.pop(); // [sizhuo] pop processed msg
	// [sizhuo] if msgQ not empty, we should try to process next msg at next cycle
	if(!msgQ.empty()) {
		doFirstMsgCB::schedule(1, this);
	}
}
