
#ifndef CACHE_INPORT_H
#define CACHE_INPORT_H

#include "estl.h"
#include "GStats.h"
#include "MemObj.h"
#include "Snippets.h"
#include "callback.h"
#include "Port.h"
#include "MemRequest.h"
#include <queue>

// base class for cache inports containing msg from upper & lower levels
class CacheInport {
protected:
	// [sizhuo] port for new msg to contend for enqueue
	PortGeneric *enqPort;

	// [sizhuo] memory msg (the handlers, e.g. MemObj::doReq + mem req) 
	// we use StaticCallbackBase becase 
	// 1. the object will not be destroyed in call()
	// 2. MemObj::redoReq ... are StaticCallbackMember0

	// [sizhuo] do real work to enq callback to msgQ 
	// and try to process it if it is the only msg in msgQ
	// and record inport pointer into msg
	virtual void enqMsgQ(StaticCallbackBase *cb) = 0;
	// [sizhuo] callback for enqMsgQ
	typedef CallbackMember1<CacheInport, StaticCallbackBase*, &CacheInport::enqMsgQ> enqMsgQCB;

public:
	//virtual void setup(const char* name) = 0;
	CacheInport() : enqPort(0) {}
	virtual ~CacheInport() {}

	// [sizhuo] enq new msg, enq is actually done after a delay
	virtual void enqNewMsg(StaticCallbackBase *cb, bool statsFlag) = 0;
	// [sizhuo] deq processed msg
	virtual void deqDoneMsg() = 0;

	// XXX: msg handler (e.g. MemObj::doReq) should schedule itself as callback
	// when failing to process the msg
	// XXX: msg handler should call deqDoneMsg() when it processed a msg successfully
};

// [sizhuo] FIFO inport
class FIFOCacheInport : public CacheInport {
private:
	// FIFO of all pending msg
	std::queue<StaticCallbackBase*> msgQ;
	// [sizhuo] call the msg handler to handle the first msg
	void doFirstMsg(); 
	// [sizhuo] to schedule doFirstMsg as callback
	typedef CallbackMember0<FIFOCacheInport, &FIFOCacheInport::doFirstMsg> doFirstMsgCB;

	virtual void enqMsgQ(StaticCallbackBase *cb);
	
public:
	//virtual void setup(const char* name);
	FIFOCacheInport() {}
	FIFOCacheInport(const char* name) {
		// [sizhuo] fully piplined port
		enqPort = PortGeneric::create(name, 1, 1);
		I(enqPort);
	}
	virtual ~FIFOCacheInport() {}

	virtual void enqNewMsg(StaticCallbackBase *cb, bool statsFlag);
	virtual void deqDoneMsg();
};

// [sizhuo] OOO inport: msg gets handled out of order 
class OOOCacheInport : public CacheInport {
public:
	virtual void enqMsgQ(StaticCallbackBase *cb) {
		cb->call();
	}

public:
	OOOCacheInport(const char* name) {
		// [sizhuo] fully piplined port
		enqPort = PortGeneric::create(name, 1, 1);
		I(enqPort);
	}
	virtual ~OOOCacheInport() {}

	virtual void enqNewMsg(StaticCallbackBase *cb, bool statsFlag) {
		Time_t when = enqPort->nextSlot(statsFlag); 
		I(when >= globalClock);
		// [sizhuo] scheule enq
		enqMsgQCB::scheduleAbs(when, this, cb);
	}
	virtual void deqDoneMsg() {}
};

#endif
