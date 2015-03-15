
#ifndef ACACHE_H
#define ACACHE_H

#include "estl.h"
#include "CacheCore.h"
#include "GStats.h"
#include "MemObj.h"
#include "MemorySystem.h"
#include "MSHR.h"
#include "Snippets.h"
#include "CacheInport.h"

// [sizhuo] cache with store atomicity ("A" -- atomic)
class ACache : public MemObj {
private:
	// [sizhuo] input ports 
	CacheInport **reqFromUpPort; // [sizhuo] upgrade req
	CacheInport **respFromUpPort; // [sizhuo] downgrade resp
	CacheInport *fromDownPort; // [sizhuo] downgrade req & upgrade resp
	// [sizhuo] XXX: upper node for L1$ is considered as LSU here
	// but router->upper_node is empty

protected:
	const TimeDelta_t tagDelay;
	const TimeDelta_t dataDelay;
	const TimeDelta_t goUpDelay;
	const TimeDelta_t goDownDelay;

	const bool isL1;
	const bool isLLC;

	const int upNodeNum; // number of upper level of nodes

public:
	ACache(MemorySystem *gms, const char *descr_section, const char *name = NULL);
	virtual ~ACache() {}

	// Entry points to schedule that may schedule a do
	// [sizhuo] these functions are called only once for each arriving msg
	void req(MemRequest *req);
	void reqAck(MemRequest *req);
	void setState(MemRequest *req);
	void setStateAck(MemRequest *req);
	void disp(MemRequest *req);

	// This do the real work
	// [sizhuo] these functions may be called multiple times for each arriving msg
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
