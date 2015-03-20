#ifndef INDEX_SPLIT_MSHR_BANK_H
#define INDEX_SPLIT_MSHR_BANK_H

#include "HierMSHR.h"
#include "pool.h"

// [sizhuo] MSHR bank allows parallelism when req is to different cache set
// Up & Down req are stored in split MSHR entry arrays
//
// For upgrade req, it is issued only when no existing req on same cache set
// and total number of req on same cache set is less than associativity
// 
// For downgrade req, it is issued when no existing upgrade req 
// on same cache line and at Req/Ack state.
class IndexSplitMSHRBank : public MSHRBank {
private:
	const int bankID;
	char *name;
	const CacheArray *cache; // [sizhuo] pointer to cache array

	// [sizhuo] upgrade req entry
	class UpReqEntry {
	public:
		AddrType lineAddr;
		UpReqState state;
		const MemRequest *mreq;

		UpReqEntry() : lineAddr(0), state(Sleep) , mreq(0) {}
		void clear() {
			lineAddr = 0;
			state = Sleep;
			mreq = 0;
		}
	};
	// [sizhuo] pool of upgrade req entries
	pool<UpReqEntry> upReqPool;
	// [sizhuo] max number of upgrade req entries
	const int maxUpReqNum;
	// [sizhuo] number of free entries for upgrade req
	int freeUpReqNum;
	// [sizhuo] 1 cache set at most has 1 upgrade req
	// invert table: cache index -> upgrade req entry
	typedef HASH_MAP<AddrType, UpReqEntry*> Index2UpReqMap;
	Index2UpReqMap index2UpReq;

	// [sizhuo] downgrade req entry
	class DownReqEntry {
	public:
		AddrType lineAddr;
		DownReqState state;
		const MemRequest *mreq;

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
	// [sizhuo] 1 cache line at most has 1 downgrade req 
	// invert table: line addr -> downgrade req entry
	typedef HASH_MAP<AddrType, DownReqEntry*> Line2DownReqMap;
	Line2DownReqMap line2DownReq;

	// [sizhuo] number of up+down req in same cache set: index -> req num
	typedef HASH_MAP<AddrType, int> Index2ReqNumMap;
	Index2ReqNumMap index2ReqNum;

	// [sizhuo] add req consists of 2 phases: insert to MSHR & issue for handling
	void insertUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq) { MSG("ERROR"); I(0); }
	typedef CallbackMember3<IndexSplitMSHRBank, AddrType, StaticCallbackBase*, const MemRequest*, &IndexSplitMSHRBank::insertUpReq> insertUpReqCB;

	void insertDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq){ MSG("ERROR"); I(0); }
	typedef CallbackMember3<IndexSplitMSHRBank, AddrType, StaticCallbackBase*, const MemRequest*, &IndexSplitMSHRBank::insertDownReq> insertDownReqCB;

	void issueUpReq(UpReqEntry *en, StaticCallbackBase *cb){ MSG("ERROR"); I(0); }
	typedef CallbackMember2<IndexSplitMSHRBank, UpReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::issueUpReq> issueUpReqCB;
		
	void issueDownReq(DownReqEntry *en, StaticCallbackBase *cb){ MSG("ERROR"); I(0); }
	typedef CallbackMember2<IndexSplitMSHRBank, DownReqEntry*, StaticCallbackBase*, &IndexSplitMSHRBank::issueDownReq> issueDownReqCB;

	// [sizhuo] contention port for insert & issue
	PortGeneric *insertPort;
	PortGeneric *issuePort;

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
	void processPendInsertAll(); // SetState + Req

	// [sizhuo] pending issue downgrade req
	class PendIssueDownReq {
	public:
		DownReqEntry *en;
		StaticCallbackBase *cb;
		PendIssueDownReq() : en(0), cb(0) {}
		PendIssueDownReq(DownReqEntry *e, StaticCallbackBase *c) : en(e), cb(c) {}
	};
	typedef std::queue<PendIssueDownReq> PendIssueDownQ;
	PendIssueDownQ *pendIssueDownQ;
	PendIssueDownQ *callIssueDownQ;

	// [sizhuo] pending issue upgrade req
	class PendIssueUpReq {
	public:
		UpReqEntry *en;
		StaticCallbackBase *cb;
		PendIssueUpReq() : en(0), cb(0) {}
		PendIssueUpReq(UpReqEntry *e, StaticCallbackBase *c) : en(e), cb(c) {}
	};
	typedef std::queue<PendIssueUpReq> PendIssueUpQ;
	PendIssueUpQ *pendIssueUpQ;
	PendIssueUpQ *callIssueUpQ;

	void processPendIssueDown() { MSG("ERROR"); I(0); }
	void processPendIssueAll() { MSG("ERROR"); I(0); }

public:
	IndexSplitMSHRBank(int id, int upSize, int downSize, CacheArray *c, const char *str);
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