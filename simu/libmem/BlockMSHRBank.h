#ifndef BLOCK_MSHR_BANK_H
#define BLOCK_MSHR_BANK_H

#include "HierMSHR.h"

// [sizhuo] blocking MSHR
class BlockMSHRBank : public MSHRBank {
private:
	const int bankID;
	const char *name;
	const CacheArray *cache; // [sizhuo] pointer to cache array

	// [sizhuo] upgrade req entry
	class UpReqEntry {
	public:
		bool valid;
		AddrType lineAddr;
		UpReqState state;
		const MemRequest *mreq;

		UpReqEntry() : valid(false), lineAddr(0), state(Req) , mreq(0) {}
		void clear() {
			valid = false;
			lineAddr = 0;
			state = Req;
			mreq = 0;
		}
	};
	UpReqEntry upReq;

	// [sizhuo] downgrade req entry
	class DownReqEntry {
	public:
		bool valid;
		AddrType lineAddr;
		const MemRequest *mreq;

		DownReqEntry() : valid(false), lineAddr(0), mreq(0) {}
		void clear() {
			valid = false;
			lineAddr = 0;
			mreq = 0;
		}
	};
	DownReqEntry downReq;

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

	// [sizhuo] FIFO of pending dowgrade req (SetState) that try to be inserted to MSHR
	// a SetState msg is pending because 
	// 1. MSHR downReq entry is occupied
	// 2. MSHR contains upgrade req to same cache line in Req/Ack state
	PendQ *pendDownReqQ;

	// [sizhuo] FIFO of pending upgrade req that try to be inserted to MSHR
	// a Req msg is pending because 
	// 1. MSHR upReq entry is occupied
	// 2. MSHR contains downgrade/upgrade req to the same cache set
	PendQ *pendUpReqQ;

	// [sizhuo] when MSHR changes, we may invoke all pending msg
	// but pending Q may also be enqueued during this process
	// so we first move pending Q to callQ, and invoke all msg in callQ
	PendQ *callQ;

	// [sizhuo] virtual functions
	////////

	// [sizhuo] when MSHR entry state changes, we process pending msg
	// by calling addUp/DownReq()
	void processPendDownReq(); // only SetState 
	void processPendAll(); // SetState + Req
	//typedef CallbackMember0<BlockMSHRBank, &BlockMSHRBank::processPendDownReq> processPendDownReqCB;
	//typedef CallbackMember0<BlockMSHRBank, &BlockMSHRBank::processPendAll> processPendAllCB;

public:
	BlockMSHRBank(int id, CacheArray *c, const char *str);
	virtual ~BlockMSHRBank();

	// [sizhuo] virtual functions
	virtual bool addDownReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq);
	virtual void retireDownReq(AddrType lineAddr);

	virtual bool addUpReq(AddrType lineAddr, StaticCallbackBase *cb, const MemRequest *mreq);
	virtual void upReqToWait(AddrType lineAddr);
	virtual void upReqToAck(AddrType lineAddr);
	virtual void retireUpReq(AddrType lineAddr);
	////////
};

#endif
