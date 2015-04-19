#ifndef INDEX_SPLIT_MSHR_BANK_H
#define INDEX_SPLIT_MSHR_BANK_H

#include "HierMSHR.h"
#include "pool.h"

// [sizhuo] MSHR bank allows parallelism when req is to different cache set
// Up & Down req are stored in split MSHR entry arrays
//
// For upgrade req, it is issued only when 
// 1. no existing up req on same cache set
// 2. number of down req on same cache set < associativity
// (this ensures up req can always occupy a cache line)
// (we only need down req, because up req num must be 0 now)
// 
// For downgrade req, it is issued when 
// 1. no existing down req to same cache line
// 2. no existing up req at Req state to same cache SET
// (because we don't know which cache line up req is replacing)
// 3. no existing up req at Ack state to sace cache line
class IndexSplitMSHRBank : public MSHRBank {
private:
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

	// [sizhuo] process pend issue up req blocked by too many down req in same cache set
	// when a down req with such index retires
	void processPendIssueUpByReqNum(AddrType index);

	// [sizhuo] process pend up req blocked by certain active req
	void processPendIssueUp(PendIssueUpQ& q);

	// [sizhuo] upgrade req entry
	class UpReqEntry {
	public:
		AddrType lineAddr;
		UpReqState state;
		const MemRequest *mreq;
		Time_t missStartTime; // [sizhuo] time when sending req to lower level

		// [sizhuo] pending issue up/down req blocked by this req
		PendIssueDownQ pendIssueDownQ;
		PendIssueUpQ pendIssueUpQ;

		UpReqEntry() : lineAddr(0), state(Sleep) , mreq(0), missStartTime(0) {}
		void clear() {
			lineAddr = 0;
			state = Sleep;
			mreq = 0;
			missStartTime = 0;
		}
	};
	// [sizhuo] pool of upgrade req entries
	pool<UpReqEntry> upReqPool;
	// [sizhuo] max number of upgrade req entries
	const int maxUpReqNum;
	// [sizhuo] number of free entries for upgrade req
	int freeUpReqNum;
	// [sizhuo] 1 cache set at most has 1 ACTIVE upgrade req
	// invert table: cache index -> ACTIVE upgrade req entry
	typedef HASH_MAP<AddrType, UpReqEntry*> Index2UpReqMap;
	Index2UpReqMap index2UpReq;

	// [sizhuo] downgrade req entry
	class DownReqEntry {
	public:
		AddrType lineAddr;
		DownReqState state;
		const MemRequest *mreq;

		// [sizhuo] pending issue up/down req blocked by this req
		PendIssueDownQ pendIssueDownQ;
		PendIssueUpQ pendIssueUpQ;

		DownReqEntry() : lineAddr(0), state(Inactive), mreq(0) {}
		void clear() {
			lineAddr = 0;
			state = Inactive;
			mreq = 0;
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

	// [sizhuo] number of down req in same cache set: index -> down req num
	typedef HASH_MAP<AddrType, int> Index2DownNumMap;
	Index2DownNumMap index2DownNum;

	// [sizhuo] add req consists of 2 phases: insert to MSHR & issue for handling
	// insert success when there is free entry, deq msg from inport, try issue next cycle
	// if fail, msg is enq into pendInsertQ for retry
	//
	// The first input doWork: if true, do insert, 
	// else consult contention port to schedule itself as callback
	void insertUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *p, const MemRequest *mreq);
	typedef CallbackMember4<IndexSplitMSHRBank, AddrType, StaticCallbackBase*, CacheInport*, const MemRequest*, &IndexSplitMSHRBank::insertUpReq> insertUpReqCB;

	void insertDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *p, const MemRequest *mreq);
	typedef CallbackMember4<IndexSplitMSHRBank, AddrType, StaticCallbackBase*, CacheInport*, const MemRequest*, &IndexSplitMSHRBank::insertDownReq> insertDownReqCB;

	// [sizhuo] issue success when the conditions described at top is satisfied
	// and will call handler next cycle
	// if fail, msg is enq ino pendIssueQ for retry
	//
	// The first input doWork: if true, do insert, 
	// else consult contention port to schedule itself as callback
	void issueUpReq(UpReqEntry *en, StaticCallbackBase *cb);
	typedef CallbackMember2<IndexSplitMSHRBank, UpReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::issueUpReq> issueUpReqCB;
		
	void issueDownReq(DownReqEntry *en, StaticCallbackBase *cb);
	typedef CallbackMember2<IndexSplitMSHRBank, DownReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::issueDownReq> issueDownReqCB;

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
	typedef CallbackMember2<IndexSplitMSHRBank, UpReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::scheduleIssueUpReq> scheduleIssueUpReqCB;
		
	void scheduleIssueDownReq(DownReqEntry *en, StaticCallbackBase *cb) {
		I(en);
		I(en->mreq);
		Time_t when = issueDownPort->nextSlot(en->mreq->getStatsFlag());
		I(when >= globalClock);
		issueDownReqCB::scheduleAbs(when, this, en, cb);
	}
	typedef CallbackMember2<IndexSplitMSHRBank, DownReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::scheduleIssueDownReq> scheduleIssueDownReqCB;

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

	GStatsCntr *nUpInsertFail; // [sizhuo] number of failed insertion of up req to MSHR
	GStatsCntr *nDownInsertFail; // [sizhuo] number of failed insertion of down req to MSHR
	GStatsAvg **avgMissLat; // [sizhuo] average miss latency, for up req only

public:
	IndexSplitMSHRBank(int id, int upSize, int downSize, CacheArray *c, const char *str, GStatsCntr *upInsFail, GStatsCntr *downInsFail, GStatsAvg **missLat);
	virtual ~IndexSplitMSHRBank();

	// [sizhuo] virtual functions
	virtual void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq);
	virtual void retireDownReq(AddrType lineAddr);

	virtual void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq);
	virtual void upReqToWait(AddrType lineAddr);
	virtual void upReqToAck(AddrType lineAddr);
	virtual void retireUpReq(AddrType lineAddr);
	////////
};

#endif
