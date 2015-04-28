#ifndef HIER_MSHR
#define HIER_MSHR

#include "estl.h"
#include <queue>

#include "callback.h"
#include "nanassert.h"
#include "GStats.h"
#include "MemRequest.h"
#include "CacheArray.h"
#include "CacheInport.h"

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

	// [sizhuo] downgrade req state
	typedef enum {
		Inactive,
		Active
	} DownReqState;

public:
	typedef enum {
		Insert,
		Downgrade,
		Upgrade,
		Replace,
		ReqNum,
		MaxIssueSC
	} IssueStallCause;

	MSHRBank() {}
	virtual ~MSHRBank() {}

	// [sizhuo] add downgrade req to MSHR, and get it READY FOR PROCESS
	// when req is inserted to MSHR, inport is dequeued
	// when req is ready for process, callback cb will be scheduled 
	virtual void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) = 0;
	// [sizhuo] retire downgrade req from MSHR, invoke pending req
	virtual void retireDownReq(AddrType lineAddr) = 0;

	// [sizhuo] callback to retire downgrade req from MSHR
	typedef CallbackMember1<MSHRBank, AddrType, &MSHRBank::retireDownReq> retireDownReqCB;

	// [sizhuo] add upgrade req to MSHR, and get it READY FOR PROCESS
	// when req is inserted to MSHR, inport is dequeued
	// when req is ready for process, callback cb will be scheduled 
	virtual void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *inport, const MemRequest *mreq) = 0;
	// [sizhuo] upgrade req replaces an cache line
	virtual void upReqReplace(AddrType lineAddr, AddrType repLineAddr) = 0;
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
	char *name;

	CacheArray *cache; // [sizhuo] cache array

	MSHRBank **bank; // [sizhuo] bank array

	
	GStatsAvg *avgMissLat[ma_MAX]; // [sizhuo] average miss latency, for up req only
	GStatsCntr *insertFail[ma_MAX]; // [sizhuo] number of failed insertion of up/down req to MSHR
	GStatsCntr *issueFail[ma_MAX][MSHRBank::MaxIssueSC];
	GStatsCntr *issueStall[ma_MAX][MSHRBank::MaxIssueSC]; // [sizhuo] latency from insert to issue
	GStatsAvg *handleLat[ma_MAX]; // [sizhuo] latency of handling each req


	// [sizhuo] let MSHRBank to be friend class
	friend class IndexSplitMSHRBank;
	friend class FullSplitMSHRBank;

	// [sizhuo] get bank id of req
	inline uint32_t getBank(AddrType lineAddr) {
		return lineAddr & bankMask;
	}

public:
	HierMSHR(uint32_t bkNum, int bkUpSize, int bkDownSize, CacheArray *c, const char *str);
	~HierMSHR();

	const char *getName() const { return name; }

	// [sizhuo] the functions provided are almost the same as MSHRBank
	// we simply add latency param to some functions, instead of using callback
	
	void addDownReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *port, const MemRequest *mreq) {
		bank[getBank(lineAddr)]->addDownReq(lineAddr, cb, port, mreq);
	}
	void retireDownReq(AddrType lineAddr, TimeDelta_t lat) {
		MSHRBank::retireDownReqCB::schedule(lat, bank[getBank(lineAddr)], lineAddr);
	}

	void addUpReq(AddrType lineAddr, StaticCallbackBase *cb, CacheInport *port, const MemRequest *mreq) {
		bank[getBank(lineAddr)]->addUpReq(lineAddr, cb, port, mreq);
	}
	void upReqReplace(AddrType lineAddr, AddrType repLineAddr) {
		bank[getBank(lineAddr)]->upReqReplace(lineAddr, repLineAddr);
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
