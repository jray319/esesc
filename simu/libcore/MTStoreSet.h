#ifndef MT_STORE_SET_H
#define MT_STORE_SET_H

#include "estl.h"
#include "GStats.h"
#include "callback.h"
#include "DInst.h"

// [sizhuo] base class for store set in multi-core
class MTStoreSet {
public:
	MTStoreSet() {}
	virtual ~MTStoreSet() {}
	// [sizhuo] insert to store set when dinst is enq to ROB
	// 1. create dependency of dinst with previous memory inst
	// 2. set last fetched memory inst
	virtual void insert(DInst *dinst) = 0;
	// [sizhuo] dinst finishes execution
	// 1. remove the dependency of younger inst
	// 2. may reset last fetched memory inst
	virtual void remove(DInst *dinst) = 0;
	// [sizhuo] when memory dependency violation happens, train store set
	virtual void memDepViolate(DInst *oldInst, DInst *youngInst) = 0;
	// [sizhuo] reset when ROB flush
	virtual void reset() = 0;
	virtual bool isReset() = 0;
};

// [sizhuo] serialize all memory instruction
class NaiveMTStoreSet : public MTStoreSet {
private:
	DInst *lastFetchInst; // [sizhuo] last fetched mem inst
public:
	NaiveMTStoreSet() : lastFetchInst(0) {}
	virtual ~NaiveMTStoreSet() {}

	virtual void insert(DInst *dinst) {
		I(dinst);
		// [sizhuo] create dependency
		if(lastFetchInst) {
			lastFetchInst->addMemDep(dinst);
		}
		// [sizhuo] set last fetch inst
		lastFetchInst = dinst;
	}

	virtual void remove(DInst *dinst) {
		if(dinst == lastFetchInst) {
			lastFetchInst = 0;
		}
	}

	virtual void memDepViolate(DInst *oldInst, DInst *youngInst) {
		I(oldInst);
		I(youngInst);
		I(youngInst->getID() > oldInst->getID());
		I(0); // [sizhuo] should not have violation
	}

	virtual void reset() {
		I(0); // [sizhuo] should not have violation
		lastFetchInst = 0;
	}

	virtual bool isReset() { return lastFetchInst == 0; }
};

#endif
