#ifndef CACHE_ARRAY_H
#define CACHE_ARRAY_H

#include "MRouter.h"

// [sizhuo] MESI coherence cache line base class
class CacheLine {
public:
	typedef enum {I, S, E, M} MESI;
	MESI state; // [sizhuo] MESI state of this cache line
	MESI *dir; // [sizhuo] directory for cache line in upper level
	AddrType lineAddr;
	const MemRequest *upReq; // [sizhuo] upgrade req operating on this line
	const MemRequest *downReq; // [sizhuo] downgrade req operating on this line
	CacheLine() : state(I), dir(0), lineAddr(0), upReq(0), downReq(0) {}
	CacheLine(const int upNodeNum) : state(I), dir(0), lineAddr(0), upReq(0), downReq(0) {
		dir = new MESI[upNodeNum];
		I(dir);
		for(int i = 0; i < upNodeNum; i++) {
			dir[i] = I;
		}
	}
	~CacheLine() {
		if(dir) {
			delete[]dir;
		}
	}

	// [sizhuo] whether the current cache line state can satisfy the upgrade req
	static bool compatibleUpReq(MESI mesi, MsgAction act, bool isLLC) {
		switch(act) {
			case ma_setValid:
				return mesi != I;
			case ma_setExclusive:
				if(isLLC) {
					return mesi != I;
				} else {
					return mesi == E || mesi == M;
				}
			case ma_setDirty:
				if(isLLC) {
					return mesi != I;
				} else {
					return mesi == E || mesi == M;
				}
			default:
				I(0);
				MSG("ERROR: Unknown msg action %d", act);
				return true;
		}
	}

	// [sizhuo] whether the current cache line state can satisfy the downgrade req
	static bool compatibleDownReq(MESI mesi, MsgAction act) {
		switch(act) {
			case ma_setInvalid:
				return mesi == I;
			case ma_setShared:
				return mesi == I || mesi == S;
			default:
				I(0);
				MSG("ERROR: Unknown msg action %d", act);
				return true;
		}
	}


	// [sizhuo] upgraded state
	static MESI upgradeState(MsgAction act) {
		switch(act) {
			case ma_setValid:
				return S;
			case ma_setExclusive:
				return E;
			case ma_setDirty:
				return M;
			default:
				I(0);
				MSG("ERROR: Unknown msg action %d", act);
				return I;
		}
	}

	// [sizhuo] type of the ack msg with current state & req action
	static MsgAction ackAction(MESI mesi, MsgAction act) {
		switch(act) {
			case ma_setInvalid:
				return ma_setInvalid;
			case ma_setShared:
				return mesi == I ? ma_setInvalid : ma_setShared;
			case ma_setValid:
				return act;
			case ma_setExclusive:
				return act;
			case ma_setDirty:
				return act;
			default:
				I(0);
				MSG("ERROR: Unknown msg action %d", act);
				return act;
		}
	}
};

// [sizhuo] base class for all cache arrays
class CacheArray {
public:
  const uint32_t  size; // [sizhuo] cache size in byte
  const uint32_t  lineSize; // [sizhuo] cache line size in byte
  const uint32_t  assoc; // [sizhuo] set associativity

  const uint32_t  setNum; // [sizhuo] number of cache sts

  const uint32_t  log2Assoc;
  const uint64_t  log2LineSize;
  const uint64_t  maskAssoc;
  const uint32_t  maskSets; // [sizhuo] mask for index calculation
  const uint32_t  log2Sets;

	CacheArray(const uint32_t sz, const uint32_t lineSz, const uint32_t a)
		: size(sz)
		, lineSize(lineSz)
		, assoc(a)
		, setNum(size / lineSize / assoc)
		, log2Assoc(log2i(assoc))
		, log2LineSize(log2i(lineSize))
		, maskAssoc(assoc - 1)
		, maskSets(setNum - 1)
		, log2Sets(log2i(setNum))
	{
		I(size > 0);
		I(lineSize > 0);
		I(assoc > 0);
		I(setNum > 0);
		I((0x01L << log2i(size)) == size);
		I((0x01L << log2LineSize) == lineSize);
		I((0x01L << log2Assoc) == assoc);
		I((0x01L << log2Sets) == setNum);
	}

	virtual ~CacheArray() {}

	// [sizhuo] functions to get line addr & index
	AddrType getLineAddr (AddrType byteAddr) const {
		return byteAddr >> log2LineSize;
	}
	AddrType getIndex (AddrType lineAddr) const {
		return lineAddr & maskSets;
	}
	AddrType getTag (AddrType lineAddr) const {
		return lineAddr >> log2Sets;
	}

	virtual CacheLine *downReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) = 0;
	virtual CacheLine *upReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) = 0;
	// [sizhuo] find cache line: this must be called before mreq is recycled
	// and is only called by up req/reqAck
	virtual CacheLine *upReqFindLine(AddrType lineAddr, const MemRequest *mreq) = 0;
};

class LRUCacheArray : public CacheArray {
private:
	// [sizhuo] head of list -- LRU; tail of list -- MRU
	typedef std::list<CacheLine*> CacheSet;
	CacheSet *tags;

public:
	LRUCacheArray(const uint32_t sz, const uint32_t lineSz, const uint32_t a, const int upNodeNum);
	virtual ~LRUCacheArray();

	virtual CacheLine *downReqOccupyLine(AddrType lineAddr, const MemRequest *mreq);
	virtual CacheLine *upReqOccupyLine(AddrType lineAddr, const MemRequest *mreq);
	virtual CacheLine *upReqFindLine(AddrType lineAddr, const MemRequest *mreq);
};

#endif
