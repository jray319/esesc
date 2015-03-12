
#ifndef ACACHE_H
#define ACACHE_H

#include "estl.h"
#include "CacheCore.h"
#include "GStats.h"
#include "MemObj.h"
#include "MemorySystem.h"
#include "MSHR.h"
#include "Snippets.h"

// [sizhuo] cache with store atomicity ("A" -- atomic)
class ACache : public MemObj {
protected:
	const TimeDelta_t tagDelay;
	const TimeDelta_t dataDelay;
	const TimeDelta_t goUpDelay;
	const TimeDelta_t goDownDelay;

public:
	ACache(MemorySystem *gms, const char *descr_section, const char *name = NULL);
	virtual ~ACache();

	// Entry points to schedule that may schedule a do
	void req(MemRequest *req);
	void reqAck(MemRequest *req);
	void setState(MemRequest *req);
	void setStateAck(MemRequest *req);
	void disp(MemRequest *req);

	// This do the real work
	void doReq(MemRequest *req);
	void doReqAck(MemRequest *req);
	void doSetState(MemRequest *req);
	void doSetStateAck(MemRequest *req);
	void doDisp(MemRequest *req);

  TimeDelta_t ffread(AddrType addr);
  TimeDelta_t ffwrite(AddrType addr);

	bool isBusy(AddrType addr) const;
};

#endif
