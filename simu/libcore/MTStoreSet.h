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

	// [sizhuo] creator function
	static MTStoreSet *create(int32_t cpu_id);
};

// [sizhuo] store set without any forcing any dependency
class EmptyMTStoreSet : public MTStoreSet {
public:
	EmptyMTStoreSet() {}
	virtual ~EmptyMTStoreSet() {}
	virtual void insert(DInst *dinst) {}
	virtual void remove(DInst *dinst) {}
	virtual void memDepViolate(DInst *oldInst, DInst *youngInst) {}
	virtual void reset() {}
	virtual bool isReset() { return true; }
};

// [sizhuo] serialize all memory instruction
class SerialMTStoreSet : public MTStoreSet {
private:
	DInst *lastFetchInst; // [sizhuo] last fetched mem inst
public:
	SerialMTStoreSet() : lastFetchInst(0) {}
	virtual ~SerialMTStoreSet() {}

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

// [sizhuo] real store set
// -1 is invalid SSID
class FullMTStoreSet : public MTStoreSet {
private:
	SSID_t *ssit; // [sizhuo] Store Set Id Table
	DInst **lfmt; // [sizhuo] Last Fetched Memory inst Table
	static const SSID_t invalidSSID; // [sizhuo] -1 is invalid SSID
	const bool orderLdLd; // [sizhuo] ordering loads to same addr
	const uint32_t ssitSize;
	const uint32_t ssitMask;
	const uint32_t lfmtSize;
	const uint32_t lfmtMask;
	const Time_t clearCycle;

	void clear();
	StaticCallbackMember0<FullMTStoreSet, &FullMTStoreSet::clear> clearCB;

	uint32_t getSSITIndex(DInst *dinst) {
		return (dinst->getPC() >> 2) & ssitMask;
	}

	SSID_t nextSSID;
	SSID_t createSSID() {
		SSID_t retID = nextSSID;
		nextSSID = uint32_t(nextSSID + 1) & lfmtMask;
		return retID;
	}

public:
	FullMTStoreSet(int32_t cpu_id);
	virtual ~FullMTStoreSet();

	virtual void insert(DInst *dinst);
	virtual void remove(DInst *dinst);
	virtual void memDepViolate(DInst *oldInst, DInst *youngInst);
	virtual void reset();
	virtual bool isReset();
};

#endif
