#include "IndexSplitMSHRBank.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

IndexSplitMSHRBank::IndexSplitMSHRBank(int id, int upSize, int downSize, CacheArray *c, const char *str)
	: bankID(id)
	, name(0)
	, cache(c)
	, upReqPool(upSize, "IndexSplitMSHRBank_upReqPool")
	, maxUpReqNum(upSize)
	, freeUpReqNum(upSize)
	, downReqPool(downSize, "IndexSplitMSHRBank_downReqPool")
	, maxDownReqNum(downSize)
	, freeDownReqNum(downSize)
	, insertDownPort(0)
	, insertUpPort(0)
	, issueDownPort(0)
	, issueUpPort(0)
	, pendInsertDownQ(0)
	, pendInsertUpQ(0)
	, callInsertQ(0)
	, pendIssueDownQ(0)
	, callIssueDownQ(0)
	, pendIssueUpQ(0)
	, callIssueUpQ(0)
{
	I(name);
	I(cache);
	I(upSize > 0);
	I(downSize > 0);

	name = new char[strlen(str) + 50];
	I(name);
	sprintf(name, "%s_bank(%d)", str, bankID);

	pendInsertDownQ = new PendInsertQ;
	I(pendInsertDownQ);

	pendInsertUpQ = new PendInsertQ;
	I(pendInsertUpQ);

	callInsertQ = new PendInsertQ;
	I(callInsertQ);

	pendIssueDownQ = new PendIssueDownQ;
	I(pendIssueDownQ);

	callIssueDownQ = new PendIssueDownQ;
	I(callIssueDownQ);

	pendIssueUpQ = new PendIssueUpQ;
	I(pendIssueUpQ);

	callIssueUpQ = new PendIssueUpQ;
	I(callIssueUpQ);

	// [sizhuo] create insert & issue ports, all fully pipelined
	char *portName = 0;
	portName = new char[strlen(name) + 50];

	sprintf(portName, "%s_insertDownPort", name);
	insertDownPort = PortGeneric::create(portName, 1, 1);
	I(insertDownPort);

	sprintf(portName, "%s_insertUpPort", name);
	insertUpPort = PortGeneric::create(portName, 1, 1);
	I(insertUpPort);

	sprintf(portName, "%s_issueDownPort", name);
	issueDownPort = PortGeneric::create(portName, 1, 1);
	I(issueDownPort);

	sprintf(portName, "%s_issueUpPort", name);
	issueUpPort = PortGeneric::create(portName, 1, 1);
	I(issueUpPort);

	MSG("%s: maxUpReqNum %d, maxDownReqNum %d", name, maxUpReqNum, maxDownReqNum);
}

IndexSplitMSHRBank::~IndexSplitMSHRBank() {
	if(pendInsertDownQ) delete pendInsertDownQ;
	if(pendInsertUpQ) delete pendInsertUpQ;
	if(callInsertQ) delete callInsertQ;
	if(pendIssueDownQ) delete pendIssueDownQ;
	if(callIssueDownQ) delete callIssueDownQ;
	if(pendIssueUpQ) delete pendIssueUpQ;
	if(callIssueUpQ) delete callIssueUpQ;
	if(name) delete[]name;
}

void IndexSplitMSHRBank::insertDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(cb);
	I(inport);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s: insertDownReq", name));

	bool success = false;

	if(freeDownReqNum > 0) {
		// [sizhuo] have free entry insert success
		success = true;
		ID(mreq->dump("success"));
	} else {
		I(freeDownReqNum == 0);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: freeDownReqNum %d", freeDownReqNum));
	}

	if(success) {
		// [sizhuo] decrement free num
		freeDownReqNum--;
		I(freeDownReqNum >= 0);
		// [sizhuo] create entry
		DownReqEntry *en = downReqPool.out();
		I(en);
		en->clear();
		en->lineAddr = lineAddr;
		en->state = Inactive; // [sizhuo] start as Inactive
		en->mreq = mreq;
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] next cycle we schedule issue for contention
		scheduleIssueDownReqCB::schedule(1, this, en, cb);
	} else {
		// [sizhuo] insert fail, enq to pend insert Q
		I(pendInsertDownQ);
		pendInsertDownQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
	}
}

void IndexSplitMSHRBank::issueDownReq(DownReqEntry *en, StaticCallbackBase *cb) {
	I(en);
	I(en->mreq);
	ID(const MemRequest *const mreq = en->mreq);
	I(cb);
	ID(GMSG(mreq->isDebug(), "%s: issueDownReq", name));

	const AddrType lineAddr = en->lineAddr;
	const AddrType index = cache->getIndex(lineAddr);

	bool success = false;

	// [sizhuo] we can issue when 
	// 1. no existing downgrade req operates on same cache line
	// 2. no existing upgrade req operates on same cache line in Ack state 
	// 3. no existing upgrade req operates on same cache SET in Req state
	//
	// [sizhuo] search for same cache line downgrade req
	Line2DownReqMap::iterator downIter = line2DownReq.find(en->lineAddr);
	if(downIter == line2DownReq.end()) {
		// [sizhuo] no same line active down req, then search for upgrade req at same index
		Index2UpReqMap::iterator upIter = index2UpReq.find(cache->getIndex(lineAddr));
		if(upIter == index2UpReq.end()) {
			// [sizhuo] no same index up req, insert success
			success = true;
			ID(mreq->dump("success"));
		} else {
			const UpReqEntry *const upReq = upIter->second;
			I(upReq);
			if(upReq->state != Req && !(upReq->lineAddr == lineAddr && upReq->state == Ack)) {
				// [sizhuo] no same line up req in Ack state 
				// and no same set up req in Req state, insert success
				success = true;
				ID(mreq->dump("success"));
			} else {
				I(upReq->state == Req || (upReq->lineAddr == lineAddr && upReq->state == Ack));
				ID(mreq->dump("fail"));
				ID(GMSG(mreq->isDebug(), "fail reason: active up req (%lx, %d)", upReq->lineAddr, upReq->state));
			}
		}
	} else {
		I(downIter->second->lineAddr == lineAddr);
		I(downIter->second->state == Active);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx, %d)", downIter->second->lineAddr, downIter->second->state));
	}

	if(success) {
		// [sizhuo] success, change to active state
		I(en->state == Inactive);
		en->state = Active;
		// [sizhuo] insert to invert table: line addr -> down req
		I(line2DownReq.find(lineAddr) == line2DownReq.end());
		line2DownReq.insert(std::make_pair<AddrType, DownReqEntry*>(lineAddr, en));
		// [sizhuo] increase cache set req num
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		// [sizhuo] fail, enq to pend issue Q
		I(pendIssueDownQ);
		pendIssueDownQ->push(PendIssueDownReq(en, cb));
	}
}

void IndexSplitMSHRBank::addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(mreq);
	Time_t when = insertDownPort->nextSlot(mreq->getStatsFlag());
	I(when >= globalClock);
	insertDownReqCB::scheduleAbs(when, this, lineAddr, cb, inport, mreq);
#if 0
	I(cb);
	I(mreq);
	I(inport);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addDownReq:", name, bankID));

	const AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	if(freeDownReqNum > 0) {
		// [sizhuo] we have free entry, then search same cache line downgrade req
		Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
		if(downIter == line2DownReq.end()) {
			// [sizhuo] no same line down req, then search for upgrade req to same cache line
			Index2UpReqMap::iterator upIter = index2UpReq.find(cache->getIndex(lineAddr));
			if(upIter == index2UpReq.end()) {
				// [sizhuo] no same index up req, insert success
				success = true;
				ID(mreq->dump("success"));
			} else {
				UpReqEntry *upReq = upIter->second;
				I(upReq);
				if(upReq->state != Req && !(upReq->lineAddr == lineAddr && upReq->state == Ack)) {
					// [sizhuo] no same line up req in Ack state 
					// and no same set up req in Req state, insert success
					success = true;
					ID(mreq->dump("success"));
				} else {
					I(upReq->lineAddr == lineAddr);
					I(upReq->state == Req || upReq->state == Ack);
					ID(mreq->dump("fail"));
					ID(GMSG(mreq->isDebug(), "fail reason: active up req (%lx, %d)", upReq->lineAddr, upReq->state));
				}
			}
		} else {
			I(downIter->second->lineAddr == lineAddr);
			I(downIter->second->mreq);
			ID(mreq->dump("fail"));
			ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx)", downIter->second->lineAddr));
		}
	} else {
		I(freeDownReqNum == 0);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: freeDownReqNum = %d", freeDownReqNum));
	}

	if(success) {
		// [sizhuo] success, create new entry
		DownReqEntry *en = downReqPool.out();
		I(en);
		en->clear();
		en->lineAddr = lineAddr;
		en->state = Active;
		en->mreq = mreq;
		// [sizhuo] decrement free num
		freeDownReqNum--;
		I(freeDownReqNum >= 0);
		// [sizhuo] insert to invert table: line addr -> down req
		I(line2DownReq.find(lineAddr) == line2DownReq.end());
		line2DownReq.insert(std::make_pair<AddrType, DownReqEntry*>(lineAddr, en));
		// [sizhuo] increase cache set req num
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		I(pendInsertDownQ);
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendInsertDownQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
	}
#endif
}

void IndexSplitMSHRBank::insertUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(cb);
	I(inport);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s: insertUpReq", name));

	bool success = false;

	if(freeUpReqNum > 0) {
		// [sizhuo] have free entry, insert success
		success = true;
		ID(mreq->dump("success"));
	} else {
		I(freeUpReqNum == 0);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: freeUpReqNum %d", freeUpReqNum));
	}

	if(success) {
		// [sizhuo] decrement free num
		freeUpReqNum--;
		I(freeUpReqNum >= 0);
		// [sizhuo] create new entry
		UpReqEntry *en = upReqPool.out();
		I(en);
		en->clear();
		en->lineAddr = lineAddr;
		en->state = Sleep; // [sizhuo] start as sleep state
		en->mreq = mreq;
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] next cycle we schedule issue for contention
		scheduleIssueUpReqCB::schedule(1, this, en, cb);
	} else {
		// [sizhuo] insert fail, enq to pend insert Q
		I(pendInsertUpQ);
		pendInsertUpQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
	}
}

void IndexSplitMSHRBank::issueUpReq(UpReqEntry *en, StaticCallbackBase *cb) {
	I(en);
	I(en->mreq);
	ID(const MemRequest *const mreq = en->mreq);
	I(cb);
	ID(GMSG(en->mreq->isDebug(), "%s: issueUpReq", name));

	const AddrType lineAddr = en->lineAddr;
	const AddrType index = cache->getIndex(lineAddr);

	bool success = false;

	// [sizhuo] we can issue when
	// 1. no existing up req in same cache set
	// 2. no existing down req to same cache line
	// 3. number of up+down req in same cache set < associativity
	// TODO: we can just use number of down req

	// [sizhuo] search for up req in same cache set
	Index2UpReqMap::iterator upIter = index2UpReq.find(index);
	if(upIter == index2UpReq.end()) {
		// [sizhuo] no up req with same index, search for down req on same line
		Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
		if(downIter == line2DownReq.end()) {
			// [sizhuo] no down req to same cache line, check req num in same cache set
			Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
			if(numIter == index2ReqNum.end()) {
				// [sizhuo] no up or down req in same cache set, success
				success = true;
				ID(mreq->dump("success"));
			} else if (numIter->second < int(cache->assoc)) {
				I(numIter->second >= 1);
				// [sizhuo] still room in cache set, success
				success = true;
				ID(mreq->dump("success"));
			} else {
				I(numIter->second >= 1);
				I(maxDownReqNum - freeDownReqNum >= int(cache->assoc));
				ID(mreq->dump("fail"));
				ID(GMSG(mreq->isDebug(), "fail reason: %d req in cache set with assoc %d", numIter->second, cache->assoc));
			}
		} else {
			I(downIter->second);
			I(downIter->second->lineAddr == lineAddr);
			I(downIter->second->state == Active);
			ID(mreq->dump("fail"));
			ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx, %d)", downIter->second->lineAddr, downIter->second->state));
		}
	} else {
		I(upIter->second);
		I(cache->getIndex(upIter->second->lineAddr) == index);
		I(upIter->second->mreq);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: active up req (%lx, %d)", upIter->second->lineAddr, upIter->second->state));
	}

	if(success) {
		// [sizhuo] success: change state to Req
		I(en->state == Sleep);
		en->state = Req;
		// [sizhuo] insert to invert table: index -> up req
		I(index2UpReq.find(index) == index2UpReq.end());
		index2UpReq.insert(std::make_pair<AddrType, UpReqEntry*>(index, en));
		// [sizhuo] increase cache set req num
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		I(pendIssueUpQ);
		pendIssueUpQ->push(PendIssueUpReq(en, cb));
	}
}

void IndexSplitMSHRBank::addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	// [sizhuo] schedule to insert up req
	I(mreq);
	Time_t when = insertUpPort->nextSlot(mreq->getStatsFlag());
	I(when >= globalClock);
	insertUpReqCB::scheduleAbs(when, this, lineAddr, cb, inport, mreq);
#if 0
	I(cb);
	I(mreq);
	I(inport);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addUpReq:", name, bankID));

	const AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when
	// 1. there is free entry
	// 2. no existing upgrade req in same cache set
	// 3. no existing downgrade req in same cache line
	// 4. number of up+down req in same cache set < associativity
	if(freeUpReqNum > 0) {
		// [sizhuo] have free entry, search for up req in same cache set
		Index2UpReqMap::iterator upIter = index2UpReq.find(index);
		if(upIter == index2UpReq.end()) {
			// [sizhuo] no up req with same index, search for down req on same line
			// FIXME: Although we expect up req to be able to occupy a cache line
			// when total number of up+down req < associtivity
			// This relies on callback at same cycle are fired in scheduling order
			Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
			if(downIter == line2DownReq.end()) {
				// [sizhuo] no down req to same cache line, check req num in same cache set
				Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
				if(numIter == index2ReqNum.end()) {
					// [sizhuo] no up or down req in same cache set, success
					success = true;
					ID(mreq->dump("success"));
				} else if (numIter->second < int(cache->assoc)) {
					I(numIter->second >= 1);
					// [sizhuo] still room in cache set, success
					success = true;
					ID(mreq->dump("success"));
				} else {
					I(numIter->second >= 1);
					I(maxDownReqNum - freeDownReqNum >= int(cache->assoc));
					ID(mreq->dump("fail"));
					ID(GMSG(mreq->isDebug(), "fail reason: %d req in cache set", numIter->second));
				}
			} else {
				I(downIter->second);
				I(downIter->second->lineAddr == lineAddr);
				I(downIter->second->mreq);
				ID(mreq->dump("fail"));
				ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx)", downIter->second->lineAddr));
			}
		} else {
			I(upIter->second);
			I(cache->getIndex(upIter->second->lineAddr) == index);
			I(upIter->second->mreq);
			ID(mreq->dump("fail"));
			ID(GMSG(mreq->isDebug(), "fail reason: active up req (%lx, %d)", upIter->second->lineAddr, upIter->second->state));
		}
	} else {
		I(freeUpReqNum == 0);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: freeUpReqNum = %d", freeUpReqNum));
	}

	if(success) {
		// [sizhuo] success, create new entry
		UpReqEntry *en = upReqPool.out();
		I(en);
		en->clear();
		en->lineAddr = lineAddr;
		en->state = Req;
		en->mreq = mreq;
		// [sizhuo] decrement free num
		freeUpReqNum--;
		I(freeUpReqNum >= 0);
		// [sizhuo] insert to invert table: index -> up req
		I(index2UpReq.find(index) == index2UpReq.end());
		index2UpReq.insert(std::make_pair<AddrType, UpReqEntry*>(index, en));
		// [sizhuo] increase cache set req num
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		I(pendInsertUpQ);
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendInsertUpQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
	}
#endif
}

void IndexSplitMSHRBank::processPendInsertDown() {
	I(callInsertQ);
	I(pendInsertDownQ);
	I(callInsertQ->empty());

	// [sizhuo] no pend req or no free entry, return
	if(pendInsertDownQ->empty() || freeDownReqNum <= 0) {
		return;
	}

	// [sizhuo] exchange callQ & pendDownReqQ, now pendQ is empty
	std::swap(callInsertQ, pendInsertDownQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when there is no downReq entry
	while(!callInsertQ->empty() && freeDownReqNum > 0) {
		PendInsertReq r = callInsertQ->front();
		callInsertQ->pop();
		addDownReq(r.lineAddr, r.cb, r.inport, r.mreq);
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callInsertQ->empty()) {
		pendInsertDownQ->push(callInsertQ->front());
		callInsertQ->pop();
	}
}

void IndexSplitMSHRBank::processPendInsertUp() {
	I(callInsertQ);
	I(callInsertQ->empty());
	I(pendInsertUpQ);

	// [sizhuo] no pend req or no free entry, return
	if(pendInsertUpQ->empty() || freeUpReqNum <= 0) {
		return;
	}

	// [sizhuo] exchange callQ & pendUpReqQ, now pendQ is empty
	std::swap(callInsertQ, pendInsertUpQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when upReq entry is occupied
	while(!callInsertQ->empty() && freeUpReqNum > 0) {
		PendInsertReq r = callInsertQ->front();
		callInsertQ->pop();
		addUpReq(r.lineAddr, r.cb, r.inport, r.mreq);
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callInsertQ->empty()) {
		pendInsertUpQ->push(callInsertQ->front());
		callInsertQ->pop();
	}
}

void IndexSplitMSHRBank::processPendIssueDown() {
	I(callIssueDownQ);
	I(callIssueDownQ->empty());
	I(pendIssueDownQ);

	// [sizhuo] no pend req, just return
	if(pendIssueDownQ->empty()) {
		return;
	}

	// [sizhuo] exchange callQ & pendQ
	std::swap(callIssueDownQ, pendIssueDownQ);
	// [sizhuo] process every req in callQ
	while(!callIssueDownQ->empty()) {
		PendIssueDownReq r = callIssueDownQ->front();
		callIssueDownQ->pop();
		scheduleIssueDownReq(r.en, r.cb);
	}
}

void IndexSplitMSHRBank::processPendIssueUp() {
	I(callIssueUpQ);
	I(callIssueUpQ->empty());
	I(pendIssueUpQ);

	// [sizhuo] no pend req, just return
	if(pendIssueUpQ->empty()) {
		return;
	}

	// [sizhuo] exchange callQ & pendQ
	std::swap(callIssueUpQ, pendIssueUpQ);
	// [sizhuo] process every req in callQ
	while(!callIssueUpQ->empty()) {
		PendIssueUpReq r = callIssueUpQ->front();
		callIssueUpQ->pop();
		scheduleIssueUpReq(r.en, r.cb);
	}
}

void IndexSplitMSHRBank::retireDownReq(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
	I(downIter != line2DownReq.end());
	DownReqEntry *en = downIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->mreq);
	// [sizhuo] recycle entry
	en->clear();
	downReqPool.in(en);
	// [sizhuo] remove from invert table
	line2DownReq.erase(downIter);
	I(line2DownReq.find(lineAddr) == line2DownReq.end());
	// [sizhuo] increment free num
	freeDownReqNum++;
	I(freeDownReqNum > 0);
	I(freeDownReqNum <= maxDownReqNum);
	// [sizhuo] reduce req num in cache set
	Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
	I(numIter != index2ReqNum.end());
	if(numIter->second > 1) {
		numIter->second--;
	} else {
		I(numIter->second == 1);
		index2ReqNum.erase(numIter);
	}
	// [sizhuo] invoke pending issue down req
	// then issue up req
	// then insert down req
	processPendIssueDown();
	processPendIssueUp();
	processPendInsertDown();
}

void IndexSplitMSHRBank::upReqToWait(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Index2UpReqMap::iterator upIter = index2UpReq.find(index);
	I(upIter != index2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Req);
	I(en->mreq);
	// [sizhuo] change state
	en->state = Wait;
	// [sizhuo] invoke pending issue down req
	processPendIssueDown();
}

void IndexSplitMSHRBank::upReqToAck(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Index2UpReqMap::iterator upIter = index2UpReq.find(index);
	I(upIter != index2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Wait || en->state == Req); // Req if cache hit
	I(en->mreq);
	// [sizhuo] change state
	en->state = Ack;
}

void IndexSplitMSHRBank::retireUpReq(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Index2UpReqMap::iterator upIter = index2UpReq.find(index);
	I(upIter != index2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Ack);
	I(en->mreq);
	// [sizhuo] recycle the entry
	en->clear();
	upReqPool.in(en);
	// [sizhuo] remove from invert table
	index2UpReq.erase(upIter);
	I(index2UpReq.find(index) == index2UpReq.end());
	// [sizhuo] increment free num
	freeUpReqNum++;
	I(freeUpReqNum > 0);
	I(freeUpReqNum <= maxUpReqNum);
	// [sizhuo] reduce req num in cache set
	Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
	I(numIter != index2ReqNum.end());
	if(numIter->second > 1) {
		numIter->second--;
	} else {
		I(numIter->second == 1);
		index2ReqNum.erase(numIter);
	}
	// [sizhuo] invoke pending issue down req
	// then issue up req
	// then insert up req
	processPendIssueDown();
	processPendIssueUp();
	processPendInsertUp();
}
