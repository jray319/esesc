#ifndef CACHE_LINE_H
#define CACHE_LINE_H

#include "MsgAction.h"
#include "DInst.h"

class MemRequest;

// [sizhuo] MESI coherence cache line base class
class CacheLine {
public:
	typedef enum {I, S, E, M} MESI;
	MESI state; // [sizhuo] MESI state of this cache line
	MESI *dir; // [sizhuo] directory for cache line in upper level
	AddrType lineAddr;
	const MemRequest *upReq; // [sizhuo] upgrade req operating on this line
	const MemRequest *downReq; // [sizhuo] downgrade req operating on this line
	// [sizhuo] for debug, add upNode num
	ID(int upNum);
	/////
	CacheLine() : state(I), dir(0), lineAddr(0), upReq(0), downReq(0) {
		ID(upNum = 0);
	}
	CacheLine(const int upNodeNum) : state(I), dir(0), lineAddr(0), upReq(0), downReq(0) {
		dir = new MESI[upNodeNum];
		I(dir);
		for(int i = 0; i < upNodeNum; i++) {
			dir[i] = I;
		}
		ID(upNum = upNodeNum);
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

	// [sizhuo] downgraded state
	static MESI downgradeState(MsgAction act) {
		switch(act) {
			case ma_setInvalid:
				return I;
			case ma_setShared:
				return S;
			default:
				I(0);
				MSG("ERROR: Unknown msg action %d", act);
				return I;
		}
	}
};

#endif
