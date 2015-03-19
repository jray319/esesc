#include "IndexSplitMSHRBank.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

IndexSplitMSHRBank::IndexSplitMSHRBank(int id, int upSize, int downSize, CacheArray *c, const char *str)
	: bankID(id)
	, name(str)
	, cache(c)
	, upReqPool(upSize, "IndexSplitMSHRBank_upReqPool")
	, maxUpReqNum(upSize)
	, freeUpReqNum(upSize)
	, downReqPool(downSize, "IndexSplitMSHRBank_downReqPool")
	, maxDownReqNum(downSize)
	, freeDownReqNum(downSize)
	, pendDownReqQ(0)
	, pendUpReqQ(0)
	, callQ(0)
{
	I(name);
	I(cache);
	I(upSize > 0);
	I(downSize > 0);

	MSG("%s_bank(%d): maxUpReqNum %d, maxDownReqNum %d", name, bankID, maxUpReqNum, maxDownReqNum);

	pendDownReqQ = new PendQ;
	I(pendDownReqQ);

	pendUpReqQ = new PendQ;
	I(pendUpReqQ);

	callQ = new PendQ;
	I(callQ);
}

IndexSplitMSHRBank::~IndexSplitMSHRBank() {
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

void IndexSplitMSHRBank::addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
	I(cb);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addDownReq:", name, bankID));

	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when 
	// 1. there is still free entry for downgrade req
	// 2. no existing downgrade req operates on same cache line
	// 3. no existing upgrade req operates on same cache line & in Req/Ack state
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
				if(upReq->lineAddr != lineAddr || upReq->state == Wait) {
					// [sizhuo] no same line up req in Req/Ack state, insert success
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
		en->mreq = mreq;
		// [sizhuo] decrement free num
		freeDownReqNum--;
		I(freeDownReqNum >= 0);
		// [sizhuo] insert to invert table
		I(line2DownReq.find(lineAddr) == line2DownReq.end());
		line2DownReq.insert(std::make_pair<AddrType, DownReqEntry*>(lineAddr, en));
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		I(pendDownReqQ);
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendDownReqQ->push(PendReq(lineAddr, cb, mreq));
	}
}

void IndexSplitMSHRBank::addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
	I(cb);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s_bank(%d) addUpReq:", name, bankID));

	const AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we can insert when
	// 1. there is free entry
	// 2. no existing upgrade req in same cache set
	// 3. no existing downgrade req in same cache line
	if(freeUpReqNum > 0) {
		// [sizhuo] have free entry, search for up req in same cache set
		Index2UpReqMap::iterator upIter = index2UpReq.find(index);
		if(upIter == index2UpReq.end()) {
			// [sizhuo] no up req with same index, search for down req on same line
			Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
			if(downIter == line2DownReq.end()) {
				// [sizhuo] no down req to same cache line, insert success
				success = true;
				ID(mreq->dump("success"));
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
		// [sizhuo] insert to invert table
		I(index2UpReq.find(index) == index2UpReq.end());
		index2UpReq.insert(std::make_pair<AddrType, UpReqEntry*>(index, en));
		// [sizhuo] call handler next cycle
		cb->schedule(1);
	} else {
		I(pendUpReqQ);
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendUpReqQ->push(PendReq(lineAddr, cb, mreq));
	}
}

void IndexSplitMSHRBank::processPendDownReq() {
	I(callQ);
	I(pendDownReqQ);
	I(callQ->empty());

	// [sizhuo] no pend req or no free entry, return
	if(pendDownReqQ->empty() || freeDownReqNum <= 0) {
		return;
	}

	// [sizhuo] exchange callQ & pendDownReqQ, now pendQ is empty
	std::swap(callQ, pendDownReqQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when there is no downReq entry
	while(!callQ->empty() && freeDownReqNum > 0) {
		PendReq r = callQ->front();
		callQ->pop();
		addDownReq(r.lineAddr, r.cb, r.mreq);
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendDownReqQ->push(callQ->front());
		callQ->pop();
	}
}

void IndexSplitMSHRBank::processPendAll() {
	// [sizhuo] first process downgrade req
	processPendDownReq();

	// [sizhuo] then process upgrade req
	I(callQ);
	I(callQ->empty());
	I(pendUpReqQ);

	// [sizhuo] no pend req or no free entry, return
	if(pendUpReqQ->empty() || freeUpReqNum <= 0) {
		return;
	}

	// [sizhuo] exchange callQ & pendUpReqQ, now pendQ is empty
	std::swap(callQ, pendUpReqQ);
	// [sizhuo] process every req in callQ
	// optimize: stop when upReq entry is occupied
	while(!callQ->empty() && freeUpReqNum > 0) {
		PendReq r = callQ->front();
		callQ->pop();
		addUpReq(r.lineAddr, r.cb, r.mreq);
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendUpReqQ->push(callQ->front());
		callQ->pop();
	}
}

void IndexSplitMSHRBank::retireDownReq(AddrType lineAddr) {
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
	// [sizhuo] invoke pending req
	processPendAll();
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
	processPendDownReq();
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
	// [sizhuo] invoke pending req
	processPendAll();
}
