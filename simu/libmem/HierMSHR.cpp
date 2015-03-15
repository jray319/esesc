#include "HierMSHR.h"
#include "CacheInport.h"
#include <algorithm>

/*
MSHRBank::MSHRBank(int sz, int assoc)
	: size(sz)
	, nFreeSize(sz)
	, cacheAssoc(assoc)
	, enryPool(sz, "MSHR bank entry pool")
	, pendSetStateQ(0)
	, pendReqQ(0)
	, callQ(0)
{
	I(sz > 0);
	entries = new Entry[sz];
	I(entries);

	pendSetStateQ = new std::queue<MemRequest*>;
	I(pendSetStateQ);

	pendReqQ = new std::queue<MemRequest*>;
	I(pendReqQ);

	callQ = new std::queue<MemRequest*>;
	I(callQ);
}

MSHRBank::~MSHRBank() {
	if(entries) {
		delete[]entries;
		entreis = 0;
	}
	if(pendSetStateQ) {
		delete pendSetStateQ;
		pendSetStateQ = 0;
	}
	if(pendReqQ) {
		delete pendReqQ;
		pendReqQ = 0;
	}
	if(callQ) {
		delete callQ;
		callQ = 0;
	}
}

bool addDownReq(MemRequest *mreq) {
	AddrType addr = mreq->getAddr();
	AddrType lineAddr = ; // TODO: use func from CacheArray
	AddrType index = ; // TODO: use func form CacheArray
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
			// [sizhuo] insert to mreq -> entry table
			// mreq can't be in this table
			I(mreq2Entry.find(mreq) == mreq2EntryMap.end());
			mreq2Entry.insert(std::make_pair<MemRequest*, Entry*>(mreq, en));
			// [sizhuo] insert to line addr -> entry table
			line2Entry.insert(std::make_pair<AddrType, Entry*>(lineAddr, en));
			// [sizhuo] increase index -> entry num table
			Index2NumMap::iterator idxIter = index2Num.find(index);
			if(idxIter == index2Num.end()) {
				index2Num.insert(std::make_pair<AddrType, int>(index, 1));
			} else {
				idxIter->second++;
			}
		} 
	} else {
		// [sizhuo] there is existing entry on same cache line
		Entry *en = sameLineIter->second;
		// [sizhuo] check the already existed entry satify following two requirements
		// 1. no existing downgrade req
		// 2. no existing upgrade req or it is in Wait state
		if(en->downgradeReq == 0 && (en->upgradeReq == 0 || en->upReqState == Wait)) {
			success = true; // [sizhuo] mark success
			// [sizhuo] change entry, but no need to change invert tables or free size
			en->downgradeReq = mreq;
		}
	}

	if(success) {
		// [sizhuo] insert to MSHR success
		mreq->pos = MemRequest::MSHR; // [sizhuo] change mreq position
		mreq->inport->deqDoneMsg(); // [sizhuo] deq req from inport
		mreq->inport = 0; // [sizhuo] clear inport field
		(mreq->redoSetStateCB).schdule(1); // [sizhuo] re-handle req next cycle
	} else {
		// [sizhuo] fail to insert to MSHR, re-enq to pend Q
		pendDownReqQ.push(mreq);
	}
	return success;
}

bool addUpReq(MemRequest *mreq) {
	AddrType addr = mreq->getAddr();
	AddrType lineAddr = ; // TODO: use func from CacheArray
	AddrType index = ; // TODO: use func form CacheArray
	bool success = false; // [sizhuo] return value, whether insert success

	// [sizhuo] we must have free entry in order to insert upgrade req
	if(nFreeSize > 0) {
		// [sizhuo] we cannot have existing entry on the same cache line
		Line2EntryMap::iterator sameLineIter = line2Entry.find(lineAddr);
		if(sameLineIter == line2Entry.end()) {
			// [sizhuo] we must have at least 1 free cache line in the cache set
			Index2NumMap::iterator idxIter = index2Num.find(index);
			int occLineNum = 0; // [sizhuo] number of occupied lines in the set
			if(idxIter != index2Num.end()) {
				occLineNum = idxIter->second;
			}
			// [sizhuo] number of occumpied lines must be less than associativity
			if(occLineNum < cacheAssoc) {
				success = true; // [sizhuo] mark success
				// [sizhuo] set up new entry
				Entry *en = entryPool.out();
				I(en);
				en->clear();
				en->upgradeReq = mreq;
				en->upReqState = Req;
				// [sizhuo] dec free entry num
				nFreeSize--;
				// [sizhuo] insert to mreq -> entry table
				// mreq can't be in this table
				I(mreq2Entry.find(mreq) == mreq2EntryMap.end());
				mreq2Entry.insert(std::make_pair<MemRequest*, Entry*>(mreq, en));
				// [sizhuo] insert to line addr -> entry table
				line2Entry.insert(std::make_pair<AddrType, Entry*>(lineAddr, en));
				// [sizhuo] increase index -> entry num table
				if(idxIter == index2Num.end()) {
					index2Num.insert(std::make_pair<AddrType, int>(index, 1));
				} else {
					idxIter->second++;
				}
			}
		}
	}

	if(success) {
		// [sizhuo] insert to MSHR success
		mreq->pos = MemRequest::MSHR; // [sizhuo] change mreq position
		mreq->inport->deqDoneMsg(); // [sizhuo] deq req from inport
		mreq->inport = 0; // [sizhuo] clear inport field
		(mreq->redoReqCB).schedule(1); // [sizhuo] re-handle req next cycle
	} else {
		// [sizhuo] fail to insert to MSHR, re-enq to pend Q
		pendUpReqQ.push(mreq);
	}
	return success;
}

void processPendDownReq() {
	I(callQ->empty());
	// [sizhuo] exchange callQ & pendDownReqQ, now pendQ is empty
	std::swap(callQ, pendDownReqQ);
	// [sizhuo] process every req in callQ
	while(!callQ->empty()) {
		MemRequest *mreq = callQ->front();
		callQ->pop();
		addDownReq(mreq);
	}
	I(callQ->empty());
}

void processPendAll() {
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
		addUpReq(mreq);
	}
	// [sizhuo] flush remaining req in callQ to pendQ
	while(!callQ->empty()) {
		pendUpReqQ.push(callQ->front());
		callQ->pop();
	}
	I(callQ->empty());
}
*/
