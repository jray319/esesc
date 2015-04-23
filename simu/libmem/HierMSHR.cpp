#include "HierMSHR.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

//#include "BlockMSHRBank.h"
#include "IndexSplitMSHRBank.h"

// HierMSHR class
HierMSHR::HierMSHR(uint32_t bkNum, int bkUpSize, int bkDownSize, CacheArray *c, const char *str)
	: bankNum(bkNum)
	, bankMask(bkNum - 1)
	, name(0)
	, cache(c)
	, bank(0)
{
	// [sizhuo] bank num must be power of 2
	I((0x01L << log2i(bankNum)) == bankNum);
	// [sizhuo] bank num & per bank size must > 0
	I(bankNum > 0);
	I(bkUpSize > 0);
	I(bkDownSize > 0);

	name = new char[strlen(str) + 20];
	I(name);
	sprintf(name, "%s_MSHR", str);

	// [sizhuo] create stats
	for(int i = 0; i < ma_MAX; i++) {
		avgMissLat[i] = 0;
		insertLat[i] = 0;
		insertFail[i] = 0;
		issueLat[i] = 0;
	}
	avgMissLat[ma_setValid] = new GStatsAvg("%s_readMissLat", name);
	avgMissLat[ma_setDirty] = new GStatsAvg("%s_writeMissLat", name);
	avgMissLat[ma_setExclusive] = new GStatsAvg("%s_prefetchMissLat", name);
	I(avgMissLat[ma_setValid]);
	I(avgMissLat[ma_setDirty]);
	I(avgMissLat[ma_setExclusive]);

	insertLat[ma_setValid] = new GStatsAvg("%s_readInsertLat", name);
	insertLat[ma_setDirty] = new GStatsAvg("%s_writeInsertLat", name);
	insertLat[ma_setExclusive] = new GStatsAvg("%s_prefetchInsertLat", name);
	insertLat[ma_setInvalid] = new GStatsAvg("%s_setInvalidInsertLat", name);
	insertLat[ma_setShared] = new GStatsAvg("%s_setSharedInsertLat", name);
	I(insertLat[ma_setValid]);
	I(insertLat[ma_setDirty]);
	I(insertLat[ma_setExclusive]);
	I(insertLat[ma_setInvalid]);
	I(insertLat[ma_setShared]);

	insertFail[ma_setValid] = new GStatsCntr("%s_readInsertFail", name);
	insertFail[ma_setDirty] = new GStatsCntr("%s_writeInsertFail", name);
	insertFail[ma_setExclusive] = new GStatsCntr("%s_prefetchInsertFail", name);
	insertFail[ma_setInvalid] = new GStatsCntr("%s_setInvalidInsertFail", name);
	insertFail[ma_setShared] = new GStatsCntr("%s_setSharedInsertFail", name);
	I(insertFail[ma_setValid]);
	I(insertFail[ma_setDirty]);
	I(insertFail[ma_setExclusive]);
	I(insertFail[ma_setInvalid]);
	I(insertFail[ma_setShared]);

	issueLat[ma_setValid] = new GStatsAvg("%s_readIssueLat", name);
	issueLat[ma_setDirty] = new GStatsAvg("%s_writeIssueLat", name);
	issueLat[ma_setExclusive] = new GStatsAvg("%s_prefetchIssueLat", name);
	issueLat[ma_setInvalid] = new GStatsAvg("%s_setInvalidIssueLat", name);
	issueLat[ma_setShared] = new GStatsAvg("%s_setSharedIssueLat", name);
	I(issueLat[ma_setValid]);
	I(issueLat[ma_setDirty]);
	I(issueLat[ma_setExclusive]);
	I(issueLat[ma_setInvalid]);
	I(issueLat[ma_setShared]);

	// [sizhuo] create banks
	bank = new MSHRBank*[bankNum];
	I(bank);
	for(uint32_t i = 0; i < bkNum; i++) {
		bank[i] = 0;
		//bank[i] = new BlockMSHRBank(i, c, name);
		bank[i] = new IndexSplitMSHRBank(this, i, bkUpSize, bkDownSize, c, name);
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
