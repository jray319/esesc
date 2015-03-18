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
		Req, // [sizhuo] try to send req to lower level
		Wait, // [sizhuo] waiting for upgrade resp from lower level
		Ack // [sizhuo] try to send resp to upper level
	} UpReqState;

public:
	MSHRBank() {}
	virtual ~MSHRBank() {}

	// [sizhuo] add downgrade req to MSHR
	// if success: req is added to MSHR, return true
	// if fail: req is enq to pendDownReqQ for retrying
	// and will call mreq->doReq immediately when succeed next time
	virtual bool addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) = 0;
	// [sizhuo] retire downgrade req from MSHR
	virtual void retireDownReq(AddrType lineAddr) = 0;

	// [sizhuo] callback to retire downgrade req from MSHR
	typedef CallbackMember1<MSHRBank, AddrType, &MSHRBank::retireDownReq> retireDownReqCB;

	// [sizhuo] add upgrade req to MSHR
	// if success: req is added to MSHR, return true
	// if fail: req is enq to pendUpReqQ for retrying
	// and will call mreq->doReq immediately when succeed next time
	virtual bool addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) = 0;
	// [sizhuo] upgrade req goes to lower level
	// and invoke pending downgrade req
	virtual void upReqToWait(AddrType lineAddr) = 0;
	// [sizhuo] change upgrade req to Ack state
	virtual void upReqToAck(AddrType lineAddr) = 0;
	// [sizhuo] retire upgrade req from MSHR
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
	
	bool addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
		return bank[getBank(lineAddr)]->addDownReq(lineAddr, cb, mreq);
	}
	void retireDownReq(AddrType lineAddr, TimeDelta_t lat) {
		MSHRBank::retireDownReqCB::schedule(lat, bank[getBank(lineAddr)], lineAddr);
	}

	bool addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) {
		return bank[getBank(lineAddr)]->addUpReq(lineAddr, cb, mreq);
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


#if 0
// [sizhuo] one bank of MSHR (fully associative)
// but only allow at most 1 upgrade req per cache set
// i.e. upgrade req is added only when no up/down req in same cache set
// TODO: make MSHR able to handle multiple req in same cache set in future
class MSHRBank {
private:
	// [sizhuo] upgrade req state
	typedef enum {
		Req, // [sizhuo] try to send req to lower level
		Wait, // [sizhuo] waiting for upgrade resp from lower level
		Ack // [sizhuo] try to send resp to upper level
	} UpgradeReqState;

	// [sizhuo] entry of MSHR, it can have 3 conditions
	// (1) only an unpgrade req
	// (2) only a downgrade req
	// (3) an upgrade req + a downgrade req, but upgrade req must be waiting for resp
	// and the upgrae resp cannot come before downgrade req is resolved
	class Entry {
	public:
		MemRequest *downgradeReq;
		MemRequest *upgradeReq;
		UpgradeReqState upReqState;
		Entry() : downgradeReq(0), upgradeReq(0), upReqState(Req){}
		void clear() {
			downgradeReq = 0;
			upgradeReq = 0;
			upReqState = Req;
		}
	};

	const int size; // [sizhuo] number of entries in this MSHR bank

	int nFreeSize; // [sizhuo] number of free entries in this bank

	CacheArray *cache; // [sizhuo] pointer to cache array

	// [sizhuo] MSHR entry pool
	pool<Entry> entryPool;

	// [sizhuo] invert table from cache line addr to entry
	typedef HASH_MAP<AddrType, Entry*> Line2EntryMap;
	Line2EntryMap line2Entry;

	// [sizhuo] invert table from cache index to number of active entries
	typedef HASH_MAP<AddrType, int> Index2NumMap;
	Index2NumMap index2Num;

	// [sizhuo] FIFO of pending dowgrade req (SetState) that try to be inserted to MSHR
	// a SetState msg is pending because 
	// 1. MSHR contains upgrade req to same cache line in Req/Ack state
	// 2. MSHR bank is full AND no upgrade req to same cacheline in Wait state
	std::queue<MemRequest*> *pendDownReqQ;

	// [sizhuo] FIFO of pending upgrade req that try to be inserted to MSHR
	// a Req msg is pending because 
	// 1. MSHR bank is full
	// 2. MSHR contains downgrade/upgrade req to the same cache set
	std::queue<MemRequest*> *pendUpReqQ;

	// [sizhuo] when MSHR changes, we may invoke all pending msg
	// but pending Q may also be enqueued during this process
	// so we first move pending Q to callQ, and invoke all msg in callQ
	std::queue<MemRequest*> *callQ;

	// [sizhuo] when MSHR entry state changes, we process pending msg
	// by calling addUp/DownReq()
	void processPendDownReq(); // only SetState 
	void processPendAll(); // SetState + Req
	typedef CallbackMember0<MSHRBank, &MSHRBank::processPendDownReq> processPendDownReqCB;
	typedef CallbackMember0<MSHRBank, &MSHRBank::processPendAll> processPendAllCB;

public:
	MSHRBank(int sz, CacheArray *c);
	~MSHRBank();

};
#endif


#endif
