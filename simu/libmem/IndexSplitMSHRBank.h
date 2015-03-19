#ifndef INDEX_SPLIT_MSHR_BANK_H
#define INDEX_SPLIT_MSHR_BANK_H

#include "HierMSHR.h"
#include "pool.h"

// [sizhuo] MSHR bank allows parallelism when req is to different cache set
// Up & Down req are stored in split MSHR entry arrays
// For upgrade req, it is accepted only when no existing req on same cache set
// For downgrade req, it is accepted when no existing upgrade req 
// on same cache line and at Req/Ack state.
class IndexSplitMSHRBank : public MSHRBank {
private:
	const int bankID;
	const char *name;
	const CacheArray *cache; // [sizhuo] pointer to cache array

	// [sizhuo] upgrade req entry
	class UpReqEntry {
	public:
		AddrType lineAddr;
		UpReqState state;
		const MemRequest *mreq;

		UpReqEntry() : lineAddr(0), state(Req) , mreq(0) {}
		void clear() {
			lineAddr = 0;
			state = Req;
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
		const MemRequest *mreq;

		DownReqEntry() : lineAddr(0), mreq(0) {}
		void clear() {
			lineAddr = 0;
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

	// [sizhuo] pending req
	class PendReq {
	public:
		AddrType lineAddr;
		StaticCallbackBase *cb;
		const MemRequest *mreq;
		PendReq() : lineAddr(0), cb(0), mreq(0) {}
		PendReq(AddrType lineA, StaticCallbackBase *c, const MemRequest *r) 
			: lineAddr(lineA)
			, cb(c)
			, mreq(r)
			{}
	};
	typedef std::queue<PendReq> PendQ;
	PendQ *pendDownReqQ;
	PendQ *pendUpReqQ;
	PendQ *callQ;

	void processPendDownReq(); // only SetState 
	void processPendAll(); // SetState + Req

public:
	IndexSplitMSHRBank(int id, int upSize, int downSize, CacheArray *c, const char *str);
	virtual ~IndexSplitMSHRBank();

	// [sizhuo] virtual functions
	virtual void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq);
	virtual void retireDownReq(AddrType lineAddr);

	virtual void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq);
	virtual void upReqToWait(AddrType lineAddr);
	virtual void upReqToAck(AddrType lineAddr);
	virtual void retireUpReq(AddrType lineAddr);
	////////
};

#endif
