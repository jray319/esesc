#ifndef WMMRESOURCE_H
#define WMMRESOURCE_H

#include "GStats.h"
#include "Resource.h"
#include "nanassert.h"
#include "callback.h"

class WMMFURALU : public Resource {
  GStatsCntr memoryBarrier;
  Time_t blockUntil;

protected:
public:
  WMMFURALU(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, int32_t id);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst *dinst, bool flushing);
  bool       retire(DInst    *dinst, bool flushing);
  void       performed(DInst *dinst);
	// [sizhuo] no need for recovery from ROB flush
	virtual void flush() {}
};

class WMMLSResource : public Resource {
protected:
	MemObj *DL1;
	GMemorySystem *memorySystem;
	LSQ *lsq;
	StoreSet *storeset;
	
public:
	WMMLSResource(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t id);
};

class WMMFULoad : public WMMLSResource {
private:
  const TimeDelta_t LSDelay; // [sizhuo] store to load forwarding delay
	const int32_t maxEntries; // [sizhuo] used to recover freeEntries in ROB flush
  int32_t freeEntries;

protected:
  void cacheDispatched(DInst *dinst);
  typedef CallbackMember1<WMMFULoad, DInst *, &WMMFULoad::cacheDispatched> cacheDispatchedCB;

public:
	WMMFULoad(Cluster *cls, PortGeneric *aGen, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
	// [sizhuo] recover freeEntry when ROB flush
	virtual void flush() {
		freeEntries = maxEntries;
	}
};

class WMMFUStore : public WMMLSResource {
private:
	const int32_t maxEntries; // [sizhuo] used to recover freeEntries in ROB flush
  int32_t freeEntries;

protected:
  void cacheDispatched(DInst *dinst);
  typedef CallbackMember1<WMMFUStore, DInst *, &WMMFUStore::cacheDispatched> cacheDispatchedCB;

public:
	WMMFUStore(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
	// [sizhuo] recover freeEntry when ROB flush
	virtual void flush() {
		freeEntries = maxEntries;
	}
};

#endif
