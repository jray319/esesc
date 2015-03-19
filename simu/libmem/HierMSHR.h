#ifndef HIER_MSHR
#define HIER_MSHR

#include "estl.h"
#include <queue>

#include "callback.h"
#include "nanassert.h"
#include "GStats.h"
#include "MemRequest.h"
#include "CacheArray.h"

// [sizhuo] base class of 1 bank of MSHR
class MSHRBank {
protected:
	// [sizhuo] upgrade req state
	typedef enum {
		Sleep, // [sizhuo] just inserted to MSHR, not activated
		Req, // [sizhuo] try to send req to lower level
		Wait, // [sizhuo] waiting for upgrade resp from lower level
		Ack // [sizhuo] try to send resp to upper level
	} UpReqState;

public:
	MSHRBank() {}
	virtual ~MSHRBank() {}

	// [sizhuo] add downgrade req to MSHR
	// if success: req is added to MSHR, and is READY FOR PROCESS, schedule callback next cycle
	// if fail: req will be retried, and will schedule callback at the next cycle of success
	virtual void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) = 0;
	// [sizhuo] retire downgrade req from MSHR, invoke pending req
	virtual void retireDownReq(AddrType lineAddr) = 0;

	// [sizhuo] callback to retire downgrade req from MSHR
	typedef CallbackMember1<MSHRBank, AddrType, &MSHRBank::retireDownReq> retireDownReqCB;

	// [sizhuo] add upgrade req to MSHR
	// if success: req is added to MSHR, and is READY FOR PROCESS, schedule callback next cycle
	// if fail: req will be retried, and will schedule callback at the next cycle of success
	virtual void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) = 0;
	// [sizhuo] upgrade req goes to lower level, invoke pending req
	virtual void upReqToWait(AddrType lineAddr) = 0;
	// [sizhuo] change upgrade req to Ack state
	virtual void upReqToAck(AddrType lineAddr) = 0;
	// [sizhuo] retire upgrade req from MSHR, invoke pending req
	virtual void retireUpReq(AddrType lineAddr) = 0;
	
	// [sizhuo] callback to change upgrade req to Wait state
	typedef CallbackMember1<MSHRBank, AddrType, &MSHRBank::upReqToWait> upReqToWaitCB;
	// [sizhuo] callback to retire upgrade req from MSHR
	typedef CallbackMember1<MSHRBank, AddrType, &MSHRBank::retireUpReq> retireUpReqCB;
};

// [sizhuo] banked MSHR, each bank is fully associative 
// bank is mapped by cache index
class HierMSHR {
private:
	const uint32_t bankNum; // [sizhuo] number of banks
	const uint32_t bankMask;
	const int bankSize; // [sizhuo] number of entries in one bank
	char *name;

	CacheArray *cache; // [sizhuo] cache array

	MSHRBank **bank; // [sizhuo] bank array

	// [sizhuo] get bank id of req
	inline uint32_t getBank(AddrType lineAddr) {
		return lineAddr & bankMask;
	}

public:
	HierMSHR(uint32_t bkNum, int bkSize, CacheArray *c, const char *str);
	~HierMSHR();

	const char *getName() const { return name; }

	// [sizhuo] the functions provided are almost the same as MSHRBank
	// we simply add latency param to some functions, instead of using callback
	
	void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
		bank[getBank(lineAddr)]->addDownReq(lineAddr, cb, mreq);
	}
	void retireDownReq(AddrType lineAddr, TimeDelta_t lat) {
		MSHRBank::retireDownReqCB::schedule(lat, bank[getBank(lineAddr)], lineAddr);
	}

	void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
		bank[getBank(lineAddr)]->addUpReq(lineAddr, cb, mreq);
	}
	void upReqToWait(AddrType lineAddr, TimeDelta_t lat) {
		MSHRBank::upReqToWaitCB::schedule(lat, bank[getBank(lineAddr)], lineAddr);
	}
	void upReqToAck(AddrType lineAddr) {
		bank[getBank(lineAddr)]->upReqToAck(lineAddr);
	}
	void retireUpReq(AddrType lineAddr, TimeDelta_t lat) {
		MSHRBank::retireUpReqCB::schedule(lat, bank[getBank(lineAddr)], lineAddr);
	}
};

#endif
