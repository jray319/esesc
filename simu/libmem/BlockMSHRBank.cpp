#include "BlockMSHRBank.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

// MSHRBank class
BlockMSHRBank::BlockMSHRBank(int id, CacheArray *c, const char *str)
	: bankID(id)
	, name(str)
	, cache(c)
	, pendDownReqQ(0)
	, pendUpReqQ(0)
	, callQ(0)
{
	I(c);

	pendDownReqQ = new PendQ;
	I(pendDownReqQ);

	pendUpReqQ = new PendQ;
	I(pendUpReqQ);

	callQ = new PendQ;
	I(callQ);
}

BlockMSHRBank::~BlockMSHRBank() {
	if(pendDownReqQ) {
		delete pendDownReqQ;
		pendDownReqQ = 0;
	}
	if(pendUpReqQ) {
		delete pendUpReqQ;
		pendUpReqQ = 0;
	}
	if(callQ) {
		delete callQ;
		callQ = 0;
	}
}

bool BlockMSHRBank::addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
	I(cb);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addDownReq:", name, bankID));

	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when
	// 1. downReq entry is empty
	// 2. upReq entry is invalid, or to different cache line, or in Wait state
	if(!downReq.valid && (!upReq.valid || upReq.lineAddr != lineAddr || upReq.state == Wait)) {
		// [sizhuo] insert success
		success = true;
		// [sizhuo] set up entry
		downReq.valid = true;
		downReq.lineAddr = lineAddr;
		downReq.mreq = mreq;

		ID(mreq->dump("success"));
	} else {
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: upReq = (%d, %lx, %d), downReq = (%d, %lx)", upReq.valid, upReq.lineAddr, upReq.state, downReq.valid, downReq.lineAddr));
	}

	if(!success) {
		I(pendDownReqQ);
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendDownReqQ->push(PendReq(lineAddr, cb, mreq));
	}
	return success;
}

bool BlockMSHRBank::addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
	I(cb);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addUpReq:", name, bankID));

	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when
	// 1. upReq entry is empty
	// 2. downReq entry is invalid, or to different cache line
	if(!upReq.valid && (!downReq.valid || downReq.lineAddr != lineAddr)) {
		// [sizhuo] insert success
		success = true;
		// [sizhuo] set up entry
		upReq.valid = true;
		upReq.lineAddr = lineAddr;
		upReq.state = Req;
		upReq.mreq = mreq;

		ID(mreq->dump("success"));
	} else {
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: upReq = (%d, %lx, %d), downReq = (%d, %lx)", upReq.valid, upReq.lineAddr, upReq.state, downReq.valid, downReq.lineAddr));
	}

	if(!success) {
		I(pendUpReqQ);
		pendUpReqQ->push(PendReq(lineAddr, cb, mreq));
	}
	return success;
}

void BlockMSHRBank::processPendDownReq() {
	I(callQ);
	I(pendDownReqQ);
	I(callQ->empty());
	if(pendDownReqQ->empty()) return;

	// [sizhuo] exchange callQ & pendDownReqQ, now pendQ is empty
	std::swap(callQ, pendDownReqQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when downReq entry is occupied
	while(!callQ->empty() && !downReq.valid) {
		PendReq r = callQ->front();
		callQ->pop();
		bool success = addDownReq(r.lineAddr, r.cb, r.mreq);
		if(success) {
			// [sizhuo] retry success, call redoSetState immediately to change state
			(r.cb)->call();
		}
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendDownReqQ->push(callQ->front());
		callQ->pop();
	}
}

void BlockMSHRBank::processPendAll() {
	// [sizhuo] first process downgrade req
	processPendDownReq();

	// [sizhuo] then process upgrade req
	I(callQ);
	I(callQ->empty());
	I(pendUpReqQ);
	if(pendUpReqQ->empty()) return;

	// [sizhuo] exchange callQ & pendUpReqQ, now pendQ is empty
	std::swap(callQ, pendUpReqQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when upReq entry is occupied
	while(!callQ->empty() && !upReq.valid) {
		PendReq r = callQ->front();
		callQ->pop();
		bool success = addUpReq(r.lineAddr, r.cb, r.mreq);
		if(success) {
			// [sizhuo] retry success, call redoReq immediately to change state
			(r.cb)->call();
		}
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendUpReqQ->push(callQ->front());
		callQ->pop();
	}
}

void BlockMSHRBank::retireDownReq(AddrType lineAddr) {
	// [sizhuo] downgrade req must match
	I(downReq.valid);
	I(downReq.lineAddr == lineAddr);
	// [sizhuo] reset entry
	downReq.clear();
	// [sizhuo] invoke pending req
	if(upReq.valid) {
		// [sizhuo] upReq exists, can only accept down req
		processPendDownReq();
	} else {
		processPendAll();
	}
}

void BlockMSHRBank::upReqToWait(AddrType lineAddr) {
	// [sizhuo] upgrade req must match
	I(upReq.valid);
	I(upReq.lineAddr == lineAddr);
	I(upReq.state == Req);
	// [sizhuo] change state & invoke pending down req
	upReq.state = Wait;
	processPendDownReq();
}

void BlockMSHRBank::upReqToAck(AddrType lineAddr) {
	// [sizhuo] upgrade req must match
	I(upReq.valid);
	I(upReq.lineAddr == lineAddr);
	// [sizhuo] state can be Req if cache hit
	I(upReq.state == Req || upReq.state == Wait);
	// [sizhuo] change state (no need to invoke pending req)
	upReq.state = Ack;
}

void BlockMSHRBank::retireUpReq(AddrType lineAddr) {
	// [sizhuo] upgrade req must match
	I(upReq.valid);
	I(upReq.lineAddr == lineAddr);
	I(upReq.state == Ack);
	// [sizhuo] reset state & invoke all pending req
	upReq.clear();
	processPendAll();
}

