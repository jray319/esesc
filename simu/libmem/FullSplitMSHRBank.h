#ifndef FULL_SPLIT_MSHR_BANK_H
#define FULL_SPLIT_MSHR_BANK_H

#include "HierMSHR.h"
#include "pool.h"

// [sizhuo] MSHR bank allows parallelism when req is to different cache LINE
// Up & Down req are stored in split MSHR entry arrays
//
// For upgrade req, it is issued only when
// 1. no existing up req on same cache line
// 2. no existing up req is REPLACING same cache line
// 3. no existing down req on same cache line
// 4. number of down + up req on same cache set < associativity
// (this ensures up req can always occupy a cache line)
// 
// For downgrade req, it is issued when
// 1. no existing down req to same cache line
// 2. no existing up req at Req state upgrading/replacing same cache line
// 3. no existing up req at Ack state to same cache line
class FullSplitMSHRBank : public MSHRBank {
private:
	HierMSHR *mshr; // [sizhuo] pointer to parent MSHR
	const int bankID;
	char *name;
	const CacheArray *cache; // [sizhuo] pointer to cache array

	// [sizhuo] forward declare of entry types
	class DownReqEntry;
	class UpReqEntry;

	// [sizhuo] pending issue downgrade req
	class PendIssueDownReq {
	public:
		DownReqEntry *en;
		StaticCallbackBase *cb;
		PendIssueDownReq() : en(0), cb(0) {}
		PendIssueDownReq(DownReqEntry *e, StaticCallbackBase *c) : en(e), cb(c) {}
	};
	typedef std::queue<PendIssueDownReq> PendIssueDownQ;
	// [sizhuo] process pend down req blocked by certain active req
	void processPendIssueDown(PendIssueDownQ& q);

	// [sizhuo] pending issue upgrade req
	class PendIssueUpReq {
	public:
		UpReqEntry *en;
		StaticCallbackBase *cb;
		PendIssueUpReq() : en(0), cb(0) {}
		PendIssueUpReq(UpReqEntry *e, StaticCallbackBase *c) : en(e), cb(c) {}
	};
	typedef std::queue<PendIssueUpReq> PendIssueUpQ;
	// [sizhuo] pending issue up req which is blocked by too many req in same cache set
	PendIssueUpQ *pendIssueUpByReqNumQ;
	// [sizhuo] callQ for fast process of pending req
	PendIssueUpQ *callIssueUpQ;

	// [sizhuo] process pend issue up req blocked by too many req in same cache set
	// when a req with such index retires
	void processPendIssueUpByReqNum(AddrType index);

	// [sizhuo] process pend up req blocked by certain active req
	void processPendIssueUp(PendIssueUpQ& q);

	// [sizhuo] upgrade req entry
	class UpReqEntry {
	public:
		AddrType lineAddr; // [sizhuo] line addr being requested
		AddrType repLineAddr; // [sizhuo] line addr being replaced by this req
		bool repAddrValid; // [sizhuo] whether the above rep addr is valid
		UpReqState state;
		const MemRequest *mreq;
		Time_t missStartTime; // [sizhuo] time when sending req to lower level
		Time_t insertTime; // [sizhuo] time when entry is created

		// [sizhuo] down req blocked by this one 
		// because down req has same req addr, or addr is being replaced by this one
		// flush when this entry goes to wait state OR retired
		PendIssueDownQ pendIssueDownQ;
		// [sizhuo] up req blocked by having same addr as this one 
		// flush when this entry retires
		PendIssueUpQ pendIssueUpRetireQ;
		// [sizhuo] up req blocked by being replaced by this one
		// flush when this entry goes to wait state
		PendIssueUpQ pendIssueUpWaitQ;

		UpReqEntry() : lineAddr(0), repLineAddr(0), repAddrValid(false), state(Sleep) , mreq(0), missStartTime(0), insertTime(0) {}
		void clear() {
			lineAddr = 0;
			repLineAddr = 0;
			repAddrValid = false;
			state = Sleep;
			mreq = 0;
			missStartTime = 0;
			insertTime = 0;
		}
	};
	// [sizhuo] pool of upgrade req entries
	pool<UpReqEntry> upReqPool;
	// [sizhuo] max number of upgrade req entries
	const int maxUpReqNum;
	// [sizhuo] number of free entries for upgrade req
	int freeUpReqNum;
	// [sizhuo] invert table: line addr -> ACTIVE upgrade req
	typedef HASH_MAP<AddrType, UpReqEntry*> Line2UpReqMap;
	Line2UpReqMap line2UpReq; // request line -> upgrade req
	Line2UpReqMap repLine2UpReq; // replace line -> upgrade req

	// [sizhuo] downgrade req entry
	class DownReqEntry {
	public:
		AddrType lineAddr;
		DownReqState state;
		const MemRequest *mreq;
		Time_t insertTime; // [sizhuo] time when entry is created

		// [sizhuo] pending issue up/down req blocked by this req
		PendIssueDownQ pendIssueDownQ;
		PendIssueUpQ pendIssueUpQ;

		DownReqEntry() : lineAddr(0), state(Inactive), mreq(0), insertTime(0) {}
		void clear() {
			lineAddr = 0;
			state = Inactive;
			mreq = 0;
			insertTime = 0;
		}
	};
	// [sizhuo] pool of downgrade req entries
	pool<DownReqEntry> downReqPool;
	// [sizhuo] max number of downgrade req entries
	const int maxDownReqNum;
	// [sizhuo] number of free entries for downgrade req
	int freeDownReqNum;
	// [sizhuo] 1 cache line at most has 1 ACTIVE downgrade req 
	// invert table: line addr -> ACTIVE downgrade req entry
	typedef HASH_MAP<AddrType, DownReqEntry*> Line2DownReqMap;
	Line2DownReqMap line2DownReq;

	// [sizhuo] number of up+down req in same cache set: index -> req num
	typedef HASH_MAP<AddrType, int> Index2ReqNumMap;
	Index2ReqNumMap index2ReqNum;

	// [sizhuo] add req consists of 2 phases: insert to MSHR & issue for handling
	// insert success when there is free entry, deq msg from inport, try issue next cycle
	// if fail, msg is enq into pendInsertQ for retry
	//
	// The first input doWork: if true, do insert, 
	// else consult contention port to schedule itself as callback
	void insertUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *p, const MemRequest *mreq);
	typedef CallbackMember4<FullSplitMSHRBank, AddrType, StaticCallbackBase*, CacheInport*, const MemRequest*, &FullSplitMSHRBank::insertUpReq> insertUpReqCB;

	void insertDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *p, const MemRequest *mreq);
	typedef CallbackMember4<FullSplitMSHRBank, AddrType, StaticCallbackBase*, CacheInport*, const MemRequest*, &FullSplitMSHRBank::insertDownReq> insertDownReqCB;

	// [sizhuo] issue success when the conditions described at top is satisfied
	// and will call handler next cycle
	// if fail, msg is enq ino pendIssueQ for retry
	//
	// The first input doWork: if true, do insert, 
	// else consult contention port to schedule itself as callback
	void issueUpReq(UpReqEntry *en, StaticCallbackBase *cb);
	typedef CallbackMember2<FullSplitMSHRBank, UpReqEntry*, StaticCallbackBase*, &FullSplitMSHRBank::issueUpReq> issueUpReqCB;
		
	void issueDownReq(DownReqEntry *en, StaticCallbackBase *cb);
	typedef CallbackMember2<FullSplitMSHRBank, DownReqEntry*, StaticCallbackBase*, &FullSplitMSHRBank::issueDownReq> issueDownReqCB;

	// [sizhuo] contention port for insert & issue
	PortGeneric *insertDownPort;
	PortGeneric *insertUpPort;
	PortGeneric *issueDownPort;
	PortGeneric *issueUpPort;

	// [sizhuo] due to contention, we need function just to schedule issue
	void scheduleIssueUpReq(UpReqEntry *en, StaticCallbackBase *cb) {
		I(en);
		I(en->mreq);
		Time_t when = issueUpPort->nextSlot(en->mreq->getStatsFlag());
		I(when >= globalClock);
		issueUpReqCB::scheduleAbs(when, this, en, cb);
	}
	typedef CallbackMember2<FullSplitMSHRBank, UpReqEntry*, StaticCallbackBase*, &FullSplitMSHRBank::scheduleIssueUpReq> scheduleIssueUpReqCB;
		
	void scheduleIssueDownReq(DownReqEntry *en, StaticCallbackBase *cb) {
		I(en);
		I(en->mreq);
		Time_t when = issueDownPort->nextSlot(en->mreq->getStatsFlag());
		I(when >= globalClock);
		issueDownReqCB::scheduleAbs(when, this, en, cb);
	}
	typedef CallbackMember2<FullSplitMSHRBank, DownReqEntry*, StaticCallbackBase*, &FullSplitMSHRBank::scheduleIssueDownReq> scheduleIssueDownReqCB;

	// [sizhuo] pending insert req
	class PendInsertReq {
	public:
		AddrType lineAddr;
		StaticCallbackBase *cb;
		CacheInport *inport;
		const MemRequest *mreq;
		PendInsertReq() : lineAddr(0), cb(0), inport(0), mreq(0) {}
		PendInsertReq(AddrType lineA, StaticCallbackBase *c, CacheInport *p, const MemRequest *r) 
			: lineAddr(lineA)
			, cb(c)
			, inport(p)
			, mreq(r)
			{}
	};
	typedef std::queue<PendInsertReq> PendInsertQ;
	PendInsertQ *pendInsertDownQ;
	PendInsertQ *pendInsertUpQ;
	PendInsertQ *callInsertQ;

	void processPendInsertDown(); // only SetState 
	void processPendInsertUp(); // SetState + Req

public:
	FullSplitMSHRBank(HierMSHR *m, int id, int upSize, int downSize, CacheArray *c, const char *str);
	virtual ~FullSplitMSHRBank();

	// [sizhuo] virtual functions
	virtual void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq);
	virtual void retireDownReq(AddrType lineAddr);

	virtual void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq);
	virtual void upReqReplace(AddrType lineAddr, AddrType repLineAddr);
	virtual void upReqToWait(AddrType lineAddr);
	virtual void upReqToAck(AddrType lineAddr);
	virtual void retireUpReq(AddrType lineAddr);
	////////
};

#endif
