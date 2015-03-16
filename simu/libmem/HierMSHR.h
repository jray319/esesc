#ifndef HIER_MSHR
#define HIER_MSHR

#include "estl.h"
#include <queue>

#include "callback.h"
#include "nanassert.h"
#include "GStats.h"
#include "MemRequest.h"
#include "CacheArray.h"

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

	// [sizhuo] retire downgrade req from MSHR
	// may unmark occupied bit in cache array, and invoke pending req
	// XXX: req it self won't change, and inport won't deq this msg
	void retireDownReq(const MemRequest *mreq);

	// [sizhuo] upgrade req goes to lower level
	// and invoke pending downgrade req
	// XXX: req it self won't change, and inport won't deq this msg
	void upReqToWait(const MemRequest *mreq);

	// [sizhuo] retire upgrade req from MSHR
	// unmark occupied bit in cache array, and invoke pending req
	// XXX: req it self won't change, and inport won't deq this msg
	void retireUpReq(const MemRequest *mreq);

public:
	MSHRBank(int sz, CacheArray *c);
	~MSHRBank();

	// [sizhuo] add downgrade req to MSHR
	// if success: (1) req is added to MSHR (2) req pos is set to MSHR 
	// (3) call inport to deq this msg (4) req will be handled next cycle
	// otherwise: req is enq to pendDownReqQ, and req itself is not changed
	bool addDownReq(MemRequest *mreq);

	// [sizhuo] callback to retire downgrade req from MSHR
	typedef CallbackMember1<MSHRBank, const MemRequest*, &MSHRBank::retireDownReq> retireDownReqCB;

	// [sizhuo] add upgrade req to MSHR
	// if success: (1) req is added to MSHR (2) req pos is set to MSHR 
	// (3) call inport to deq this msg (4) req will be handled next cycle
	// otherwise: req is enq to pendDownReqQ
	bool addUpReq(MemRequest *mreq);

	// [sizhuo] callback to change upgrade req to Wait state
	typedef CallbackMember1<MSHRBank, const MemRequest*, &MSHRBank::upReqToWait> upReqToWaitCB;

	// [sizhuo] change upgrade req to Ack state
	// XXX: req it self won't change, and inport won't deq this msg
	void upReqToAck(const MemRequest *mreq);
	
	// [sizhuo] callback to retire upgrade req from MSHR
	typedef CallbackMember1<MSHRBank, const MemRequest*, &MSHRBank::retireUpReq> retireUpReqCB;
};

// [sizhuo] banked MSHR, each bank is fully associative 
// bank is mapped by cache index
class HierMSHR {
private:
	const uint32_t bankNum; // [sizhuo] number of banks
	const uint32_t bankMask;
	const int bankSize; // [sizhuo] number of entries in one bank

	CacheArray *cache; // [sizhuo] cache array

	MSHRBank **bank; // [sizhuo] bank array

	// [sizhuo] get bank id of req
	uint32_t getBank(const MemRequest *mreq) { 
		return (cache->getLineAddr(mreq->getAddr())) & bankMask;
	}

public:
	HierMSHR(uint32_t bkNum, int bkSize, CacheArray *c);
	~HierMSHR();

	// [sizhuo] the functions provided are almost the same as MSHRBank
	// we simply add latency param to some functions, instead of using callback
	
	bool addDownReq(MemRequest *mreq) {
		return bank[getBank(mreq)]->addDownReq(mreq);
	}
	void retireDownReq(const MemRequest *mreq, TimeDelta_t lat) {
		MSHRBank::retireDownReqCB::schedule(lat, bank[getBank(mreq)], mreq);
	}

	bool addUpReq(MemRequest *mreq) {
		return bank[getBank(mreq)]->addUpReq(mreq);
	}
	void upReqToWait(const MemRequest *mreq, TimeDelta_t lat) {
		MSHRBank::upReqToWaitCB::schedule(lat, bank[getBank(mreq)], mreq);
	}
	void upReqToAck(const MemRequest *mreq) {
		bank[getBank(mreq)]->upReqToAck(mreq);
	}
	void retireUpReq(const MemRequest *mreq, TimeDelta_t lat) {
		MSHRBank::retireUpReqCB::schedule(lat, bank[getBank(mreq)], mreq);
	}
};

#endif
