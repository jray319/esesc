#include "HierMSHR.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

// MSHRBank class
BlockMSHRBank::BlockMSHRBank(int id, HierMSHR *m, CacheArray *c, const char *str)
	: bankID(id)
	, name(str)
	, cache(c)
	, pendDownReqQ(0)
	, pendUpReqQ(0)
	, callQ(0)
{
	I(c);
	I(m);

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

// HierMSHR class
HierMSHR::HierMSHR(uint32_t bkNum, int bkSize, CacheArray *c, const char *str)
	: bankNum(bkNum)
	, bankMask(bkNum - 1)
	//, bankSize(1)
	, name(0)
	, cache(c)
	, bank(0)
{
	// [sizhuo] bank num must be power of 2
	I((0x01L << log2i(bankNum)) == bankNum);
	// [sizhuo] bank num & size must > 0
	I(bankNum > 0);
	//I(bankSize > 0);
	//I(bankSize > bankNum); // [sizhuo] prevent reverse num & size in creation

	name = new char[strlen(str) + 20];
	I(name);
	sprintf(name, "%s_MSHR", str);
	ID(MSG(name));

	// [sizhuo] create banks
	bank = new MSHRBank*[bankNum];
	I(bank);
	for(uint32_t i = 0; i < bkNum; i++) {
		bank[i] = 0;
		bank[i] = new BlockMSHRBank(i, this, c, name);
		I(bank[i]);
	}
}

HierMSHR::~HierMSHR() {
	if(bank) {
		for(uint32_t i = 0; i < bankNum; i++) {
			if(bank[i]) delete[]bank[i];
		}
		delete[]bank;
	}
	if(name) {
		delete[]name;
	}
}

#if 0
bool MSHRBank::addDownReq(MemRequest *mreq) {
	ID(mreq->dump("MSHR addDownReq"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);
	AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	// search entry with same line addr in MSHR
	Line2EntryMap::iterator sameLineIter = line2Entry.find(lineAddr);
	if(sameLineIter == line2Entry.end()) {
		// [sizhuo] no entry operates on the same cache line
		// we can insert only when there is free entry
		if(nFreeSize > 0) {
			success = true; // [sizhuo] mark success
			// [sizhuo] set up new entry
			Entry *en = entryPool.out();
			I(en);
			en->clear();
			en->downgradeReq = mreq;
			// [sizhuo] dec free entry num
			nFreeSize--;
			// [sizhuo] insert to line addr -> entry table
			line2Entry.insert(std::make_pair<AddrType, Entry*>(lineAddr, en));
			// [sizhuo] increase index -> entry num table
			Index2NumMap::iterator idxIter = index2Num.find(index);
			if(idxIter == index2Num.end()) {
				index2Num.insert(std::make_pair<AddrType, int>(index, 1));
			} else {
				idxIter->second++;
			}
		} else {
			ID(GMSG(mreq->isDebug(), "addDownReq fail: nFreeSize = %d", nFreeSize));
		}
	} else {
		// [sizhuo] there is existing entry on same cache line
		Entry *en = sameLineIter->second;
		// [sizhuo] check the already existed entry satify following two requirements
		// 1. no existing downgrade req
		// 2. no existing upgrade req or it is in Wait state
		if(en->downgradeReq == 0 && (en->upgradeReq == 0 || en->upReqState == Wait)) {
			success = true; // [sizhuo] mark success
			// [sizhuo] change entry, but no need to change free size or invert table
			en->downgradeReq = mreq;
		} else {
			ID(GMSG(mreq->isDebug(), "addDownReq fail: en->downgradeReq = %p, en->upgradeReq = %p, en->upReqState = %d", en->downgradeReq, en->upgradeReq, en->upReqState));
		}
	}

	if(!success) {
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendDownReqQ->push(mreq);
	}
	return success;
}

bool MSHRBank::addUpReq(MemRequest *mreq) {
	ID(mreq->dump("MSHR addUpReq"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);
	AddrType index = cache->getIndex(lineAddr);
	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we must have free entry in order to insert upgrade req
	if(nFreeSize > 0) {
		// [sizhuo] we cannot have existing entry in the same cache set
		Index2NumMap::iterator idxIter = index2Num.find(index);
		if(idxIter == index2Num.end()) {
			success = true; // [sizhuo] mark success
			// [sizhuo] set up new entry
			Entry *en = entryPool.out();
			I(en);
			en->clear();
			en->upgradeReq = mreq;
			en->upReqState = Req;
			// [sizhuo] dec free entry num
			nFreeSize--;
			// [sizhuo] insert to line addr -> entry table
			I(line2Entry.find(lineAddr) == line2Entry.end());
			line2Entry.insert(std::make_pair<AddrType, Entry*>(lineAddr, en));
			// [sizhuo] insert to index -> entry num table
			index2Num.insert(std::make_pair<AddrType, int>(index, 1));
		} else {
			// [sizhuo] we can't have 0 in index2Num
			I(idxIter->second > 0);
			ID(GMSG(mreq->isDebug(), "addUpReq fail: index req num = %d", idxIter->second));
		}
	} else {
		ID(GMSG(mreq->isDebug(), "addUpReq fail: nFreeSize = %d", nFreeSize));
	}

	if(!success) {
		// [sizhuo] fail to insert to MSHR, enq to pend Q
		pendUpReqQ->push(mreq);
	}
	return success;
}

void MSHRBank::processPendDownReq() {
	I(callQ->empty());
	// [sizhuo] exchange callQ & pendDownReqQ, now pendQ is empty
	std::swap(callQ, pendDownReqQ);
	// [sizhuo] process every req in callQ
	while(!callQ->empty()) {
		MemRequest *mreq = callQ->front();
		callQ->pop();
		bool success = addDownReq(mreq);
		if(success) { 
			// [sizhuo] retry success, call redoSetState immediately to change state
			(mreq->redoSetStateCB).call();
		}
	}
	I(callQ->empty());
}

void MSHRBank::processPendAll() {
	// [sizhuo] first process downgrade req
	processPendDownReq();

	// [sizhuo] then process upgrade req
	I(callQ->empty());
	if(nFreeSize < 1) {
		// [sizhuo] optimize: add upgrade req must need free entry
		I(nFreeSize == 0);
		return;
	}
	// [sizhuo] exchange callQ & pendUpReqQ, now pendQ is empty
	std::swap(callQ, pendUpReqQ);
	// [sizhuo] optimize: add upgrade req must need free entry
	while(!callQ->empty() && nFreeSize > 0) {
		MemRequest *mreq = callQ->front();
		callQ->pop();
		bool success = addUpReq(mreq);
		if(success) {
			// [sizhuo] retry success, call redoReq immediately to change state
			(mreq->redoReqCB).call();
		}
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendUpReqQ->push(callQ->front());
		callQ->pop();
	}
	I(callQ->empty());
}

void MSHRBank::retireDownReq(const MemRequest *mreq) {
	ID(mreq->dump("MSHR retireDownReq"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);
	AddrType index = cache->getIndex(lineAddr);

	// [sizhuo] search corresponding entry, must exist
	Line2EntryMap::iterator lineIter = line2Entry.find(lineAddr);
	I(lineIter != line2Entry.end());
	Entry *en = lineIter->second;
	I(en->downgradeReq == mreq);

	if(en->upgradeReq) {
		// [sizhuo] upgrade req pending at same cache line
		I(en->upReqState == Wait);
		en->downgradeReq = 0; // [sizhuo] clear downgrade req
		// [sizhuo] no need to change any invert table or free size
		// because upgrade req still exists
		// We only need to invoke pending downgrade req (actually uneccessary)
		processPendDownReqCB::schedule(1, this);
	} else {
		// [sizhuo] no upgrade req pending, we can recycle whole entry
		en->clear();
		entryPool.in(en);
		// increase free size
		nFreeSize++;
		// [sizhuo] remove from line addr -> entry table
		line2Entry.erase(lineIter);
		// [sizhuo] decrease index -> req num table item
		Index2NumMap::iterator idxIter = index2Num.find(index);
		I(idxIter != index2Num.end() && idxIter->second >= 1);
		if(idxIter->second > 1) {
			idxIter->second--;
		} else {
			index2Num.erase(idxIter);
		}
		// [sizhuo] process all pending req
		processPendAllCB::schedule(1, this);
	}
}

void MSHRBank::upReqToWait(const MemRequest *mreq) {
	ID(mreq->dump("MSHR upReqToWait"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);

	// [sizhuo] search corresponding entry, must exist
	Line2EntryMap::iterator lineIter = line2Entry.find(lineAddr);
	I(lineIter != line2Entry.end());
	Entry *en = lineIter->second;
	I(en->upgradeReq == mreq);
	
	// [sizhuo] change state, no need to change free size or invert table
	I(en->upReqState == Req);
	en->upReqState = Wait;

	// [sizhuo] invoke pending downgrade req
	processPendDownReqCB::schedule(1, this);
}

void MSHRBank::upReqToAck(const MemRequest *mreq) {
	ID(mreq->dump("MSHR upReqToAck"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);

	// [sizhuo] search corresponding entry, must exist
	Line2EntryMap::iterator lineIter = line2Entry.find(lineAddr);
	I(lineIter != line2Entry.end());
	Entry *en = lineIter->second;
	I(en->upgradeReq == mreq);
	
	// [sizhuo] state can be Req if cache hit
	I(en->upReqState == Wait || en->upReqState == Req); 
	// [sizhuo] downgrade req to same cache line must not exist
	I(en->downgradeReq == 0);

	// [sizhuo] change state, no need to change free size or invert table
	en->upReqState = Ack;

	// [sizhuo] no need to invoke pending req
}

void MSHRBank::retireUpReq(const MemRequest *mreq) {
	ID(mreq->dump("MSHR retireUpReq"));

	AddrType addr = mreq->getAddr();
	AddrType lineAddr = cache->getLineAddr(addr);
	AddrType index = cache->getIndex(lineAddr);

	// [sizhuo] search corresponding entry, must exist
	Line2EntryMap::iterator lineIter = line2Entry.find(lineAddr);
	I(lineIter != line2Entry.end());
	Entry *en = lineIter->second;
	I(en->upgradeReq == mreq);
	
	// [sizhuo] we can recycle this entry
	I(en->upReqState == Ack);
	I(en->downgradeReq == 0);
	en->clear();
	entryPool.in(en);
	// [sizhuo] increase free size
	nFreeSize++;
	// [sizhuo] remove from line -> entry table
	line2Entry.erase(lineIter);
	// [sizhuo] decrement index -> req num table item
	Index2NumMap::iterator idxIter = index2Num.find(index);
	I(idxIter != index2Num.end() && idxIter->second >= 1);
	if(idxIter->second > 1) {
		idxIter->second--;
	} else {
		index2Num.erase(idxIter);
	}

	// [sizhuo] invoke all pending req
	processPendAllCB::schedule(1, this);
}

#endif
