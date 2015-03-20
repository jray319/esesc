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
	, insertPort(0)
	, issuePort(0)
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

	/*
	pendIssueDownQ = new PendIssueDownQ;
	I(pendIssueDownQ);

	callIssueDownQ = new PendIssueDownQ;
	I(callIssueDownQ);

	pendIssueUpQ = new PendIssueUpQ;
	I(pendIssueUpQ);

	callIssueUpQ = new PendIssueUpQ;
	I(callIssueUpQ);
	*/

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

void IndexSplitMSHRBank::addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(cb);
	I(mreq);
	I(inport);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addDownReq:", name, bankID));

	const AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when 
	// 1. there is still free entry for downgrade req
	// 2. no existing downgrade req operates on same cache line
	// 3. no existing upgrade req operates on same cache line in Ack state 
	// 4. no existing upgrade req operates on same cache SET in Req state
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
}

void IndexSplitMSHRBank::addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
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

void IndexSplitMSHRBank::processPendInsertAll() {
	// [sizhuo] first process downgrade req
	processPendInsertDown();

	// [sizhuo] then process upgrade req
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
	// [sizhuo] invoke pending req
	processPendInsertAll();
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
	// [sizhuo] invoke pending downgrade req
	processPendInsertDown();
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
	// [sizhuo] invoke pending req
	processPendInsertAll();
}
