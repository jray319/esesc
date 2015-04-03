
#include <limits.h>
#include "Resource.h"
#include "WMMResource.h"

#include "Cluster.h"
#include "DInst.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "GProcessor.h"
#include "WMMProcessor.h"
#include "MemRequest.h"
#include "Port.h"
#include "LSQ.h"

// [sizhuo] WMMFURALU class: for fences & syscall
WMMFURALU::WMMFURALU(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, int32_t id)
	: Resource(cls, aGen, l)
	, memoryBarrier("P(%d)_memoryBarrier", id)
{
	blockUntil = 0;
}

StallCause WMMFURALU::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  I(dinst->getPC() != 0xf00df00d); // It used to be a Syspend, but not longer true

  if (dinst->getPC() == 0xdeaddead){ 
    // This is the PC for a syscall (QEMUReader::syscall)
    if (blockUntil==0) {
      blockUntil = globalClock+100; // [sizhuo] fix delay for syscall -- 100
      return SyscallStall;
    }
    
    //is this where we poweron the GPU threads and then poweroff the QEMU thread?
    if (globalClock >= blockUntil) {
      blockUntil = 0;
      return NoStall;
    }
    
    return SyscallStall;
  } else if (!dinst->getInst()->hasDstRegister() 
            && !dinst->getInst()->hasSrc1Register() 
            && !dinst->getInst()->hasSrc2Register()
            ) { 
		// [sizhuo] TODO: DMB fence, we should not have it anymore
		// it should be cracked into Commit & Reconcile fences
		I(0);
		MSG("ERROR: unexpected DMB fence");

    if (gproc->isROBEmpty())
      return NoStall;
    memoryBarrier.inc(dinst->getStatsFlag());
    return SyscallStall;
  }
  
  return NoStall;
}

void WMMFURALU::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}

void WMMFURALU::executed(DInst *dinst) {
  dinst->markExecuted();
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		return;
	}

  cluster->executed(dinst);
}

bool WMMFURALU::preretire(DInst *dinst, bool flushing) {
  return true;
}

bool WMMFURALU::retire(DInst *dinst, bool flushing) {
  return true;
}

void WMMFURALU::performed(DInst *dinst) {
  dinst->markPerformed();
  I(0); // It should be called only for memory operations
}

// WMMLSResource class: base class for LSU
WMMLSResource::WMMLSResource(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t id)
	: Resource(cls, aGen, l)
	, DL1(ms->getDL1())
	, memorySystem(ms)
	, lsq(NULL)
	, storeset(NULL)
{
  I(SescConf->getBool("cpusimu", "enableDcache", id));
	I(ms);
}

// [sizhuo] WMMFULoad class: load unit
WMMFULoad::WMMFULoad(Cluster *cls, PortGeneric *aGen, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id)
	: WMMLSResource(cls, aGen, l, ms, id)
	, LSDelay(lsdelay)
	, maxEntries(size)
	, freeEntries(size)
{
}

StallCause WMMFULoad::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	if(freeEntries <= 0) {
		I(freeEntries == 0);
		return OutsLoadsStall;
	}
	freeEntries--;
	// TODO: update store set
	return NoStall;
}

void WMMFULoad::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

	cluster->executing(dinst); // [sizhuo] just change wake up time
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	if(dinst->getInst()->isRecFence()) {
		executedCB::scheduleAbs(when, this, dinst);
	} else {
		// [sizhuo] schedule call back to access cache
		cacheDispatchedCB::scheduleAbs(when, this, dinst);
	}
}

void WMMFULoad::cacheDispatched(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

	// [sizhuo] send read req
	I(!DL1->isBusy(dinst->getAddr())); // [sizhuo] cache always available
  MemRequest::sendReqRead(DL1, dinst->getStatsFlag(), dinst->getAddr(), performedCB::create(this,dinst));
}

void WMMFULoad::performed(DInst *dinst) {
  dinst->markPerformed();
  executed(dinst);
}

void WMMFULoad::executed(DInst *dinst) {
  dinst->markExecuted();
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		return;
	}

	// TODO: wake up inst with addr dependency on dinst via store set
	
	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

bool WMMFULoad::preretire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	return true;
}

bool WMMFULoad::retire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	freeEntries++; // [sizhuo] release entry now
	return true;
}

// [sizhuo] WMMFUStore class: store unit
WMMFUStore::WMMFUStore(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id)
	: WMMLSResource(cls, aGen, l, ms, id)
	, maxEntries(size)
	, freeEntries(size)
{}

StallCause WMMFUStore::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  if (dinst->getInst()->isStoreAddress()) {
    return NoStall;
	}

	if(dinst->getInst()->isComFence()) {
		return NoStall;
	}

  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsStoresStall;
  }

	// TODO: update store set
	
  freeEntries--;
  return NoStall;
}

void WMMFUStore::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

  cluster->executing(dinst);
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	if(dinst->getInst()->isStoreAddress() || dinst->getInst()->isComFence()) {
		executedCB::scheduleAbs(when, this, dinst);
	} else {
		// [sizhuo] schedule call back to access cache
		cacheDispatchedCB::scheduleAbs(when, this, dinst);
	}
}

void WMMFUStore::cacheDispatched(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

	// [sizhuo] send write req
	I(!DL1->isBusy(dinst->getAddr())); // [sizhuo] cache always available
	MemRequest::sendReqWrite(DL1, dinst->getStatsFlag(), dinst->getAddr(), performedCB::create(this,dinst));
}

void WMMFUStore::executed(DInst *dinst) {
  dinst->markExecuted();
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		return;
	}

	if(dinst->getInst()->isStoreAddress()) {
		// [sizhuo] do prefetch
		I(!DL1->isBusy(dinst->getAddr())); // [sizhuo] cache always available
		MemRequest::sendReqWritePrefetch(DL1, dinst->getStatsFlag(), dinst->getAddr());
	}

	// TODO: wake up inst with addr dependency on dinst via store set
	
	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

void WMMFUStore::performed(DInst *dinst) {
  dinst->markPerformed();
  executed(dinst);
}

bool WMMFUStore::preretire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	return true;
}

bool WMMFUStore::retire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	freeEntries++; // [sizhuo] release entry now
	return true;
}

