#include "FullSplitMSHRBank.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

FullSplitMSHRBank::FullSplitMSHRBank(HierMSHR *m, int id, int upSize, int downSize, CacheArray *c, const char *str)
	: mshr(m)
	, bankID(id)
	, name(0)
	, cache(c)
	, pendIssueUpByReqNumQ(0)
	, callIssueUpQ(0)
	, upReqPool(upSize, "FullSplitMSHRBank_upReqPool")
	, maxUpReqNum(upSize)
	, freeUpReqNum(upSize)
	, downReqPool(downSize, "FullSplitMSHRBank_downReqPool")
	, maxDownReqNum(downSize)
	, freeDownReqNum(downSize)
	, insertDownPort(0)
	, insertUpPort(0)
	, issueDownPort(0)
	, issueUpPort(0)
	, pendInsertDownQ(0)
	, pendInsertUpQ(0)
	, callInsertQ(0)
{
	I(m);
	I(str);
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

	pendIssueUpByReqNumQ = new PendIssueUpQ;
	I(pendIssueUpByReqNumQ);

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

	MSG("%s: FullSplitMSHR, maxUpReqNum %d, maxDownReqNum %d", name, maxUpReqNum, maxDownReqNum);
}

FullSplitMSHRBank::~FullSplitMSHRBank() {
	if(pendInsertDownQ) delete pendInsertDownQ;
	if(pendInsertUpQ) delete pendInsertUpQ;
	if(callInsertQ) delete callInsertQ;
	if(pendIssueUpByReqNumQ) delete pendIssueUpByReqNumQ;
	if(callIssueUpQ) delete callIssueUpQ;
	if(name) delete[]name;
}

void FullSplitMSHRBank::insertDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
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
		en->doStats = mreq->getStatsFlag(); // [sizhuo] record stats flag
		en->action = mreq->getAction(); // [sizhuo] record action
		en->lastTime = globalClock; // [sizhuo] record insert time
		en->issueSC = Insert; // [sizhuo] set stall cause
		I((en->pendIssueDownQ).empty());
		I((en->pendIssueUpQ).empty());
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] next cycle we schedule issue for contention
		// cut down combinational path
		scheduleIssueDownReqCB::schedule(1, this, en, cb);
	} else {
		// [sizhuo] insert fail, enq to pend insert Q
		I(pendInsertDownQ);
		pendInsertDownQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
		// [sizhuo] stats: insert fail
		I(mreq->getAction() < ma_MAX);
		I(mshr->insertFail[mreq->getAction()]);
		mshr->insertFail[mreq->getAction()]->inc(mreq->getStatsFlag());
	}
}

void FullSplitMSHRBank::issueDownReq(DownReqEntry *en, StaticCallbackBase *cb) {
	I(en);
	I(en->mreq);
	ID(const MemRequest *const mreq = en->mreq);
	I(cb);
	ID(GMSG(mreq->isDebug(), "%s: issueDownReq", name));

	const AddrType lineAddr = en->lineAddr;

	bool success = false;

	// [sizhuo] stats on issue stall
	I(en->action < ma_MAX);
	I(en->issueSC < MaxIssueSC);
	I(mshr->issueStall[en->action][en->issueSC]);
	mshr->issueStall[en->action][en->issueSC]->add(globalClock - en->lastTime, en->doStats);

	// [sizhuo] record issue time
	en->lastTime = globalClock;

	// [sizhuo] we can issue when 
	// 1. no existing downgrade req operates on same cache line
	// 2. no existing up req at Req state upgrading/replacing same cache line
	// 3. no existing upgrade req operates on same cache line in Ack state
	//
	// [sizhuo] search for same cache line downgrade req
	Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
	if(downIter == line2DownReq.end()) {
		// [sizhuo] no same line active down req, then search for up req replacing same line
		Line2UpReqMap::iterator repIter = repLine2UpReq.find(lineAddr);
		if(repIter == repLine2UpReq.end()) {
			// [sizhuo] the addr is not being replaced, search for up req upgrading same line
			Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
			if(upIter == line2UpReq.end()) {
				// [sizhuo] no same line up req, insert success
				success = true;
				ID(mreq->dump("success"));
			} else {
				UpReqEntry *upReq = upIter->second;
				I(upReq);
				if(upReq->state == Wait) {
					// [sizhuo] same line up req in wait state, success
					success = true;
					ID(mreq->dump("success"));
				} else {
					I(upReq->state == Req || upReq->state == Ack);
					ID(mreq->dump("fail"));
					ID(GMSG(mreq->isDebug(), "fail reason: active up req upgrading (%lx, %lx, %d, %d)", upReq->lineAddr, upReq->repLineAddr, upReq->repAddrValid, upReq->state));
					// [sizhuo] fail, add to the pendQ of upIter
					(upReq->pendIssueDownQ).push(PendIssueDownReq(en, cb));
					// [sizhuo] set issue stall cause
					en->issueSC = Upgrade;
				}
			}
		} else {
			UpReqEntry *repReq = repIter->second;
			I(repReq);
			I(repReq->repAddrValid);
			I(repReq->repLineAddr == lineAddr);
			I(repReq->state == Req);
			ID(mreq->dump("fail"));
			ID(GMSG(mreq->isDebug(), "fail reason: active up req replacing (%lx, %lx, %d, %d)", repReq->lineAddr, repReq->repLineAddr, repReq->repAddrValid, repReq->state));
			// [sizhuo] fail, add to the pendQ of repIter
			(repReq->pendIssueDownQ).push(PendIssueDownReq(en, cb));
			// [sizhuo] set issue stall cause
			en->issueSC = Replace;
		}
	} else {
		I(downIter->second);
		I(downIter->second->lineAddr == lineAddr);
		I(downIter->second->state == Active);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx, %d)", downIter->second->lineAddr, downIter->second->state));
		// [sizhuo] fail, add to the pendQ of downIter
		(downIter->second->pendIssueDownQ).push(PendIssueDownReq(en, cb));
		// [sizhuo] set issue stall cause
		en->issueSC = Downgrade;
	}

	if(success) {
		// [sizhuo] success, change to active state
		I(en->state == Inactive);
		en->state = Active;
		en->issueSC = MaxIssueSC; // [sizhuo] clear issue stall cause
		I((en->pendIssueDownQ).empty());
		I((en->pendIssueUpQ).empty());
		// [sizhuo] insert to invert table: line addr -> down req
		I(line2DownReq.find(lineAddr) == line2DownReq.end());
		line2DownReq.insert(std::make_pair<AddrType, DownReqEntry*>(lineAddr, en));
		// [sizhuo] increase cache set req num
		const AddrType index = cache->getIndex(lineAddr);
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// XXX: [sizhuo] call handler NOW: atomically issue & occupy the cache line
		cb->call();
	} else {
		// [sizhuo] stats: issue fail
		I(en->issueSC < MaxIssueSC);
		I(mshr->issueFail[en->action][en->issueSC]);
		mshr->issueFail[en->action][en->issueSC]->inc(en->doStats);
	}
}

void FullSplitMSHRBank::addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(mreq);
	Time_t when = insertDownPort->nextSlot(mreq->getStatsFlag());
	I(when >= globalClock);
	insertDownReqCB::scheduleAbs(when, this, lineAddr, cb, inport, mreq);
}

void FullSplitMSHRBank::insertUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	I(cb);
	I(inport);
	I(mreq);
	ID(GMSG(mreq->isDebug(), "%s: insertUpReq", name));
	I(mreq->getAction() == mreq->getOrigReqAction());

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
		en->doStats = mreq->getStatsFlag(); // [sizhuo] record stats flag
		en->action = mreq->getAction(); // [sizhuo] record action
		en->lastTime = globalClock; // [sizhuo] record insert time
		en->issueSC = Insert; // [sizhuo] set stall cause
		I((en->pendIssueDownQ).empty());
		I((en->pendIssueUpWaitQ).empty());
		I((en->pendIssueUpRetireQ).empty());
		// [sizhuo] deq msg from inport
		inport->deqDoneMsg();
		// [sizhuo] next cycle we schedule issue for contention
		// cut down combinational path
		scheduleIssueUpReqCB::schedule(1, this, en, cb);
	} else {
		// [sizhuo] insert fail, enq to pend insert Q
		I(pendInsertUpQ);
		pendInsertUpQ->push(PendInsertReq(lineAddr, cb, inport, mreq));
		// [sizhuo] stats: insert fail
		I(mreq->getAction() < ma_MAX);
		I(mshr->insertFail[mreq->getAction()]);
		mshr->insertFail[mreq->getAction()]->inc(mreq->getStatsFlag());
	}
}

void FullSplitMSHRBank::issueUpReq(UpReqEntry *en, StaticCallbackBase *cb) {
	I(en);
	I(en->mreq);
	ID(const MemRequest *const mreq = en->mreq);
	I(cb);
	ID(GMSG(en->mreq->isDebug(), "%s: issueUpReq", name));

	const AddrType lineAddr = en->lineAddr;
	const AddrType index = cache->getIndex(lineAddr);

	bool success = false;

	// [sizhuo] stats on issue stall
	I(en->action < ma_MAX);
	I(en->issueSC < MaxIssueSC);
	I(mshr->issueStall[en->action][en->issueSC]);
	mshr->issueStall[en->action][en->issueSC]->add(globalClock - en->lastTime, en->doStats);

	// [sizhuo] record issue time
	en->lastTime = globalClock;

	// [sizhuo] we can issue when
	// 1. no existing up req in same cache set
	// 2. no existing up req is REPLACING same cache line
	// 3. no existing down req on same cache line
	// 4. number of down + up req on same cache set < associativity

	// [sizhuo] search for up req upgrading same cache line
	Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
	if(upIter == line2UpReq.end()) {
		// [sizhuo] addr is not being upgraded, check whether it is being replaced
		Line2UpReqMap::iterator repIter = repLine2UpReq.find(lineAddr);
		if(repIter == repLine2UpReq.end()) {
			// [sizhuo] no up req on same addr, search for down req on same line
			Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
			if(downIter == line2DownReq.end()) {
				// [sizhuo] no down req to same cache line, check req num in same cache set
				Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
				if(numIter == index2ReqNum.end()) {
					// [sizhuo] no req in same cache set, success
					success = true;
					ID(mreq->dump("success"));
				} else if (numIter->second < int(cache->assoc)) {
					I(numIter->second >= 1);
					// [sizhuo] still room in cache set, success
					success = true;
					ID(mreq->dump("success"));
				} else {
					ID(mreq->dump("fail"));
					ID(GMSG(mreq->isDebug(), "fail reason: %d req in cache set with assoc %d", numIter->second, cache->assoc));
					// [sizhuo] fail, add to pendIssueUpByReqNumQ
					I(pendIssueUpByReqNumQ);
					pendIssueUpByReqNumQ->push(PendIssueUpReq(en, cb));
					// [sizhuo] set issue stall cause
					en->issueSC = ReqNum;
				}
			} else {
				DownReqEntry *downReq = downIter->second;
				I(downReq);
				I(downReq->lineAddr == lineAddr);
				I(downReq->state == Active);
				ID(mreq->dump("fail"));
				ID(GMSG(mreq->isDebug(), "fail reason: active down req (%lx, %d)", downReq->lineAddr, downReq->state));
				// [sizhuo] fail, add to pendQ of dowIter
				(downReq->pendIssueUpQ).push(PendIssueUpReq(en, cb));
				// [sizhuo] set issue stall cause
				en->issueSC = Downgrade;
			}
		} else {
			UpReqEntry *repReq = repIter->second;
			I(repReq);
			I(repReq->repAddrValid);
			I(repReq->repLineAddr == lineAddr);
			I(repReq->state == Req);
			ID(mreq->dump("fail"));
			ID(GMSG(mreq->isDebug(), "fail reason: active up req replacing (%lx, %lx, %d, %d)", repReq->lineAddr, repReq->repLineAddr, repReq->repAddrValid, repReq->state));
			// [sizhuo] fail, add to pend wait Q of repIter
			(repReq->pendIssueUpWaitQ).push(PendIssueUpReq(en, cb));
			// [sizhuo] set issue stall cause
			en->issueSC = Replace;
		}
	} else {
		UpReqEntry *upReq = upIter->second;
		I(upReq);
		I(upReq->mreq);
		ID(mreq->dump("fail"));
		ID(GMSG(mreq->isDebug(), "fail reason: active up req upgrading (%lx, %lx, %d, %d)", upReq->lineAddr, upReq->repLineAddr, upReq->repAddrValid, upReq->state));
		// [sizhuo] fail, add to pend retire Q of upIter
		(upReq->pendIssueUpRetireQ).push(PendIssueUpReq(en, cb));
		// [sizhuo] set issue stall cause
		en->issueSC = Upgrade;
	}

	if(success) {
		// [sizhuo] success: change state to Req
		I(en->state == Sleep);
		I(!en->repAddrValid && en->repLineAddr == 0);
		en->state = Req;
		en->issueSC = MaxIssueSC; // [sizhuo] clear issue stall cause
		I((en->pendIssueDownQ).empty());
		I((en->pendIssueUpWaitQ).empty());
		I((en->pendIssueUpRetireQ).empty());
		// [sizhuo] insert to invert table: line -> up req
		I(line2UpReq.find(lineAddr) == line2UpReq.end());
		line2UpReq.insert(std::make_pair<AddrType, UpReqEntry*>(lineAddr, en));
		// [sizhuo] increase cache set req num
		Index2ReqNumMap::iterator numIter = index2ReqNum.find(index);
		if(numIter == index2ReqNum.end()) {
			index2ReqNum.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			I(numIter->second >= 1);
			numIter->second++;
		}
		// XXX: [sizhuo] call handler NOW: atomically issue & occupy the cache line
		// if this req replaces an addr, later req can see it
		cb->call();
	} else {
		// [sizhuo] stats: issue fail
		I(en->issueSC < MaxIssueSC);
		I(mshr->issueFail[en->action][en->issueSC]);
		mshr->issueFail[en->action][en->issueSC]->inc(en->doStats);
	}
}

void FullSplitMSHRBank::addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) {
	// [sizhuo] schedule to insert up req
	I(mreq);
	Time_t when = insertUpPort->nextSlot(mreq->getStatsFlag());
	I(when >= globalClock);
	insertUpReqCB::scheduleAbs(when, this, lineAddr, cb, inport, mreq);
}

void FullSplitMSHRBank::processPendInsertDown() {
	I(callInsertQ);
	I(pendInsertDownQ);
	I(callInsertQ->empty());

	// [sizhuo] no pend req or no free entry, return
	if(pendInsertDownQ->empty() || freeDownReqNum <= 0) {
		return;
	}

	// TODO: we can only process 1 pending insert req
	I(freeDownReqNum == 1);

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

void FullSplitMSHRBank::processPendInsertUp() {
	I(callInsertQ);
	I(callInsertQ->empty());
	I(pendInsertUpQ);

	// [sizhuo] no pend req or no free entry, return
	if(pendInsertUpQ->empty() || freeUpReqNum <= 0) {
		return;
	}

	// TODO: we can only process 1 pending insert req
	I(freeUpReqNum == 1);

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

void FullSplitMSHRBank::processPendIssueDown(PendIssueDownQ& pendQ) {
	while(!pendQ.empty()) {
		PendIssueDownReq r = pendQ.front();
		pendQ.pop();
		// [sizhuo] process issue next cycle
		scheduleIssueDownReqCB::schedule(1, this, r.en, r.cb);
	}
}

void FullSplitMSHRBank::processPendIssueUp(PendIssueUpQ& pendQ) {
	while(!pendQ.empty()) {
		PendIssueUpReq r = pendQ.front();
		pendQ.pop();
		// [sizhuo] process issue next cycle
		scheduleIssueUpReqCB::schedule(1, this, r.en, r.cb);
	}
}

void FullSplitMSHRBank::processPendIssueUpByReqNum(AddrType index) {
	I(callIssueUpQ);
	I(callIssueUpQ->empty());
	I(pendIssueUpByReqNumQ);

	// [sizhuo] no pend req, just return
	if(pendIssueUpByReqNumQ->empty()) {
		return;
	}

	// [sizhuo] exchange callQ & pendQ
	std::swap(callIssueUpQ, pendIssueUpByReqNumQ);
	// [sizhuo] process every req in callQ
	while(!callIssueUpQ->empty()) {
		PendIssueUpReq r = callIssueUpQ->front();
		callIssueUpQ->pop();
		// [sizhuo] only process pend req with same index
		// otherwise get back to pendQ
		I(r.en);
		if(cache->getIndex((r.en)->lineAddr) == index) {
			// [sizhuo] process issue next cycle
			scheduleIssueUpReqCB::schedule(1, this, r.en, r.cb);
		} else {
			pendIssueUpByReqNumQ->push(r);
		}
	}
}

void FullSplitMSHRBank::retireDownReq(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Line2DownReqMap::iterator downIter = line2DownReq.find(lineAddr);
	I(downIter != line2DownReq.end());
	DownReqEntry *en = downIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->mreq);
	// [sizhuo] stats: handle latency
	I(en->action < ma_MAX);
	I(mshr->handleLat[en->action]);
	mshr->handleLat[en->action]->sample(globalClock - en->lastTime, en->doStats);
	// [sizhuo] reset entry state
	en->clear();
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
	// then issue up req blocked by addr / req num
	// then insert down req
	processPendIssueDown(en->pendIssueDownQ);
	processPendIssueUp(en->pendIssueUpQ);
	processPendIssueUpByReqNum(index);
	processPendInsertDown();
	// [sizhuo] recycle entry
	I((en->pendIssueDownQ).empty());
	I((en->pendIssueUpQ).empty());
	downReqPool.in(en);
}

void FullSplitMSHRBank::upReqReplace(AddrType lineAddr, AddrType repLineAddr) {
	// [sizhuo] find the entry
	Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
	I(upIter != line2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Req);
	I(en->mreq);
	I(!en->repAddrValid && en->repLineAddr == 0);
	// [sizhuo] set replace valid bit & addr
	en->repAddrValid = true;
	en->repLineAddr = repLineAddr;
	I(lineAddr != repLineAddr);
	I(cache->getIndex(lineAddr) == cache->getIndex(repLineAddr));
	// [sizhuo] add replace addr to invert table
	I(repLine2UpReq.find(repLineAddr) == repLine2UpReq.end());
	repLine2UpReq.insert(std::make_pair<AddrType, UpReqEntry*>(repLineAddr, en));
}

void FullSplitMSHRBank::upReqToWait(AddrType lineAddr) {
	// [sizhuo] find the entry
	Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
	I(upIter != line2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Req);
	I(en->mreq);
	GI(!en->repAddrValid, (en->pendIssueUpWaitQ).empty());
	// [sizhuo] change state
	en->state = Wait;
	// [sizhuo] remove replace addr from invert table
	if(en->repAddrValid) {
		I(en->repLineAddr != lineAddr);
		I(cache->getIndex(lineAddr) == cache->getIndex(en->repLineAddr));
		Line2UpReqMap::iterator repIter = repLine2UpReq.find(en->repLineAddr);
		I(repIter != line2UpReq.end());
		I(repIter->second == en);
		repLine2UpReq.erase(repIter);
	}
	// [sizhuo] clear replace valid bit & addr
	en->repAddrValid = false;
	en->repLineAddr = 0;
	// [sizhuo] record time of forwarding req to lower level
	en->missStartTime = globalClock;
	// [sizhuo] invoke pending issue down & up req
	processPendIssueDown(en->pendIssueDownQ);
	processPendIssueUp(en->pendIssueUpWaitQ);
	I((en->pendIssueDownQ).empty());
	I((en->pendIssueUpWaitQ).empty());
}

void FullSplitMSHRBank::upReqToAck(AddrType lineAddr) {
	// [sizhuo] find the entry
	Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
	I(upIter != line2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Wait || en->state == Req); // Req if cache hit
	I(en->mreq);
	I(!en->repAddrValid && en->repLineAddr == 0);
	GI(en->state == Wait, (en->pendIssueDownQ).empty());
	I((en->pendIssueUpWaitQ).empty());
	// [sizhuo] for Wait->Ack, sample miss latency
	// since resp action may be different from original req
	// we use original req action stored in mreq to determine miss type
	// e.g. setValid --> setEx when L3 miss
	if(en->state == Wait) {
		I(globalClock > en->missStartTime);
		I(en->action < ma_MAX);
		I(mshr->avgMissLat[en->action]);
		mshr->avgMissLat[en->action]->sample(globalClock - en->missStartTime, en->doStats);
	}
	// [sizhuo] change state
	en->state = Ack;
}

void FullSplitMSHRBank::retireUpReq(AddrType lineAddr) {
	const AddrType index = cache->getIndex(lineAddr);
	// [sizhuo] find the entry
	Line2UpReqMap::iterator upIter = line2UpReq.find(lineAddr);
	I(upIter != line2UpReq.end());
	UpReqEntry *en = upIter->second;
	I(en);
	I(en->lineAddr == lineAddr);
	I(en->state == Ack);
	I(en->mreq);
	I(!en->repAddrValid && en->repLineAddr == 0);
	// [sizhuo] stats: handle latency
	I(en->action < ma_MAX);
	I(mshr->handleLat[en->action]);
	mshr->handleLat[en->action]->sample(globalClock - en->lastTime, en->doStats);
	// [sizhuo] clear entry
	en->clear();
	// [sizhuo] remove from invert table
	line2UpReq.erase(upIter);
	I(line2UpReq.find(lineAddr) == line2UpReq.end());
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
	// then issue up req blocked by addr / req num
	// then insert up req
	processPendIssueDown(en->pendIssueDownQ);
	processPendIssueUp(en->pendIssueUpRetireQ);
	processPendIssueUpByReqNum(index);
	processPendInsertUp();
	// [sizhuo] recycle the entry
	I((en->pendIssueDownQ).empty());
	I((en->pendIssueUpWaitQ).empty());
	I((en->pendIssueUpRetireQ).empty());
	upReqPool.in(en);
}
