
#ifndef CACHE_INPORT_H
#define CACHE_INPORT_H

#include "estl.h"
#include "GStats.h"
#include "MemObj.h"
#include "Snippets.h"
#include "callback.h"
#include "Port.h"
#include <queue>


// [sizhuo] input FIFO port containing msg from upper & lower levels
class CacheInport {
private:
	TimeDelta_t delay; // [sizhuo] transfer delay between cache layers

	// [sizhuo] FIFO of msg (actually the handlers, e.g. MemObj::doReq) 
	// we use StaticCallbackBase becase 
	// 1. the object will not be destroyed in call()
	// 2. MemObj::redoReq ... are StaticCallbackMember0
	std::queue<StaticCallbackBase*> msgQ;

	// [sizhuo] port for new msg to contend for enqueue
	PortGeneric *enqPort;

	// [sizhuo] do real work to enq MsgInfo to msgQ 
	// and try to process it if it is the only msg in msgQ
	void enqMsgQ(StaticCallbackBase *msg);
	// [sizhuo] callback for enqMsgQ
	typedef CallbackMember1<CacheInport, StaticCallbackBase*, &CacheInport::enqMsgQ> enqMsgQCB;

	// [sizhuo] call the msg handler to handle the first msg
	void doFirstMsg(); 
	// [sizhuo] to schedule doFirstMsg as callback
	typedef CallbackMember0<CacheInport, &CacheInport::doFirstMsg> doFirstMsgCB;
	
public:
	void setup(const TimeDelta_t lat, const char* name);
	CacheInport() : delay(0), enqPort(0) {}
	~CacheInport() {}

	// [sizhuo] enq new msg, enq is actually done after a delay
	void enqNewMsg(StaticCallbackBase *msg, bool statsFlag);
	// [sizhuo] deq processed msg
	void deqDoneMsg();

	// XXX: msg handler (e.g. MemObj::doReq) should schedule itself as callback
	// when failing to process the msg
	// XXX: msg handler should call deqDoneMsg() when it processed a msg successfully
};


#endif
