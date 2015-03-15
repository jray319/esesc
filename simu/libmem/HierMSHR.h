#ifndef HIER_MSHR
#define HIER_MSHR

#include "estl.h"
#include <queue>
//#include <unordered_map>

#include "callback.h"
#include "nanassert.h"
#include "GStats.h"
#include "MemRequest.h"

// [sizhuo] one bank of MSHR (fully associative)
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

	// [sizhuo] associativity of cache (NOT MSHR)
	// used for checking whether upgrade req can be added to MSHR
	const int cacheAssoc; 

	// [sizhuo] MSHR entry pool
	pool<Entry> entryPool;

	/*
	// [sizhuo] invert table from MemRequest to entry
	typedef std::unordered_map<MemRequest*, Entry*> Mreq2EntryMap;
	Mreq2EntryMap mreq2Entry;

	// [sizhuo] invert table from cache line addr to entry
	typedef std::unordered_map<AddrType, Entry*> Line2EntryMap;
	Line2EntryMap line2Entry;

	// [sizhuo] invert table from cache index to number of active entries
	typedef std::unordered_map<AddrType, int> Index2NumMap;
	Index2NumMap index2Num;
	*/

	// [sizhuo] FIFO of pending dowgrade req (SetState) that try to be inserted to MSHR
	// a SetState msg is pending because 
	// 1. MSHR contains upgrade req to same cache line in Req/Ack state
	// 2. MSHR bank is full AND no upgrade req to same cacheline in Wait state
	std::queue<MemRequest*> *pendDownReqQ;

	// [sizhuo] FIFO of pending upgrade req that try to be inserted to MSHR
	// a Req msg is pending because 
	// 1. MSHR bank is full
	// 2. MSHR contains downgrade/upgrade req to the same cacheline
	// 3. the cache set is all occupied
	std::queue<MemRequest*> *pendUpReqQ;

	// [sizhuo] when MSHR changes, we may invoke all pending msg
	// but pending Q may also be enqueued during this process
	// so we first move pending Q to callQ, and invoke all msg in callQ
	std::queue<MemRequest*> *callQ;

	// [sizhuo] when MSHR entry state changes, we process pending msg
	void processPendDownReq(); // only SetState 
	void processPendAll(); // SetState + Req

	// [sizhuo] retire downgrade req from MSHR, and invoke pending req
	void retireDownReq(MemRequest *mreq);

	// [sizhuo] upgrade req goes to lower level
	// and invoke pending downgrade req
	void upReqToWait(MemRequest *mreq);
	// [sizhuo] retire upgrade req from MSHR, and invoke pending req
	void retireUpReq(MemRequest *mreq);
public:
	MSHRBank(int sz, int assoc);
	~MSHRBank();

	// [sizhuo] add downgrade req to MSHR
	// if success: (1) req is added to MSHR (2) req pos is set to MSHR 
	// (3) msg is deq from inport, inport filed is cleared
	// (4) req will be handled next cycle
	// otherwise: req is enq to pendDownReqQ
	bool addDownReq(MemRequest *mreq);
	// [sizhuo] callback to retire downgrade req from MSHR
	// and invoke pending req
	typedef CallbackMember1<MSHRBank, MemRequest*, &MSHRBank::retireDownReq> retireDownReqCB;

	// [sizhuo] add upgrade req to MSHR
	// if success: (1) req is added to MSHR (2) req pos is set to MSHR 
	// (3) msg is deq from inport, inport filed is cleared
	// (4) req will be handled next cycle
	// otherwise: req is enq to pendDownReqQ
	bool addUpReq(MemRequest *mreq);
	// [sizhuo] callback to change upgrade req to Wait state
	// and invoke pending downgrade req
	typedef CallbackMember1<MSHRBank, MemRequest*, &MSHRBank::upReqToWait> upReqToWaitCB;
	// [sizhuo] change upgrade req to Ack state
	void upReqToAck(MemRequest *mreq);
	// [sizhuo] callback to retire upgrade req from MSHR
	// and invoke pending req
	typedef CallbackMember1<MSHRBank, MemRequest*, &MSHRBank::retireUpReq> retireUpReqCB;
};
#endif
