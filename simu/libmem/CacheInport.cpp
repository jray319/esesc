#include "CacheInport.h"
#include <algorithm>

void CacheInport::setup(const TimeDelta_t lat, const char* name) {
	// [sizhuo] at least 1 cycle delay, cut off combinational path
	delay = lat;
	I(delay >= 1);
	// [sizhuo] fully piplined port
	enqPort = PortGeneric::create(name, 1, 1);
	I(enqPort);
}

void CacheInport::doFirstMsg() {
	// [sizhuo] msgQ cannot be empty
	I(!msgQ.empty());
	if(msgQ.empty()) {
		MSG("WARNING: ACache inport try to handle msg when msgQ is empty");
		return;
	}
	// [sizhuo] get the first msg and call handler
	StaticCallbackBase *msg = msgQ.front();
	msg->call();
}

void CacheInport::enqMsgQ(StaticCallbackBase *msg) {
	bool initEmpty = msgQ.empty();
	msgQ.push(msg); // [sizhuo] enq
	// [sizhuo] if msgQ is initially empty, we must try to process this new msg
	if(initEmpty)	{
		msg->call();
	}
}

void CacheInport::enqNewMsg(StaticCallbackBase *msg, bool statsFlag) {
	// [sizhuo] contend for enq port
	Time_t when = enqPort->nextSlot(statsFlag); 
	I(when >= globalClock);
	// [sizhuo] scheule real enq after delay
	enqMsgQCB::scheduleAbs(delay + when, this, msg);
}

void CacheInport::deqDoneMsg() {
	I(!msgQ.empty());
	if(msgQ.empty()) {
		MSG("WARNING: ACache inport try to pop msg when msgQ is empty");
		return;
	}
	// [sizhuo] pop processed msg
	msgQ.pop();
	// [sizhuo] if msgQ not empty, we should try to process next msg at next cycle
	if(!msgQ.empty()) {
		doFirstMsgCB::schedule(1, this);
	}
}
