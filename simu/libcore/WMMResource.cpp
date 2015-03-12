
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
            ) { // [sizhuo] TODO: DMB fence, we can do better here
    if (gproc->isROBEmpty())
      return NoStall;
    memoryBarrier.inc(dinst->getStatsFlag());
    return SyscallStall;
  }
  
  return NoStall;
}

void WMMFURALU::executing(DInst *dinst) {
  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}

void WMMFURALU::executed(DInst *dinst) {
  dinst->markExecuted();
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
	, freeEntries(size)
{
}

StallCause WMMFULoad::canIssue(DInst *dinst) {
	if(freeEntries <= 0) {
		I(freeEntries == 0);
		return OutsLoadsStall;
	}
	freeEntries--;
	// TODO: update store set
	return NoStall;
}

void WMMFULoad::executing(DInst *dinst) {
	cluster->executing(dinst); // [sizhuo] just change wake up time
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	// [sizhuo] schedule call back to access cache
	cacheDispatchedCB::scheduleAbs(when, this, dinst);
}

void WMMFULoad::cacheDispatched(DInst *dinst) {
	if(DL1->isBusy(dinst->getAddr())) {
		// [sizhuo] cache is busy, schedule again
    Time_t when = gen->nextSlot(dinst->getStatsFlag());
		// [sizhuo] to stop infinite loop
		if(when == globalClock) {
			when++;
		}
		//MSG("Core %d: clock %llx, load cache dispatch fail, try again at %llx", gproc->getId(), globalClock, when);
		// [sizhuo] schedule call back to access cache again
		cacheDispatchedCB::scheduleAbs(when, this, dinst);
		return;
	}
	// [sizhuo] cache available, send read req
  MemRequest::sendReqRead(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));
}

void WMMFULoad::performed(DInst *dinst) {
  dinst->markPerformed();
  executed(dinst);
}

void WMMFULoad::executed(DInst *dinst) {
	// TODO: wake up inst with addr dependency on dinst via store set
	
  dinst->markExecuted();
	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

bool WMMFULoad::preretire(DInst *dinst, bool flushing) {
	return true;
}

bool WMMFULoad::retire(DInst *dinst, bool flushing) {
	freeEntries++; // [sizhuo] release entry now
	return true;
}

// [sizhuo] WMMFUStore class: store unit
WMMFUStore::WMMFUStore(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id)
	: WMMLSResource(cls, aGen, l, ms, id)
	, freeEntries(size)
{}

StallCause WMMFUStore::canIssue(DInst *dinst) {
  if (dinst->getInst()->isStoreAddress())
    return NoStall;

  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsStoresStall;
  }

	// TODO: update store set
	
  freeEntries--;
  return NoStall;
}

void WMMFUStore::executing(DInst *dinst) {
  cluster->executing(dinst);
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	if(dinst->getInst()->isStoreAddress()) {
		executed(dinst);
	} else {
		// [sizhuo] schedule call back to access cache
		cacheDispatchedCB::scheduleAbs(when, this, dinst);
	}
}

void WMMFUStore::cacheDispatched(DInst *dinst) {
	if(DL1->isBusy(dinst->getAddr())) {
		// [sizhuo] cache is busy, schedule again
    Time_t when = gen->nextSlot(dinst->getStatsFlag());
		// [sizhuo] to stop infinite loop
		if(when == globalClock) {
			when++;
		}
		//MSG("Core %d: clock %llx, store cache dispatch fail, try again at %llx", gproc->getId(), globalClock, when);
		// [sizhuo] schedule call back to access cache again
		cacheDispatchedCB::scheduleAbs(when, this, dinst);
		return;
	}
	// [sizhuo] cache available, send write req
	/*
	char dumpStr[100];
	sprintf(dumpStr, "Core %d issue store", gproc->getId());
	dinst->dump(dumpStr);
	*/
  MemRequest::sendReqWrite(DL1, dinst, dinst->getAddr(), executedCB::create(this,dinst));
  //executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}

void WMMFUStore::executed(DInst *dinst) {
	// TODO: wake up inst with addr dependency on dinst via store set
	//
	// [sizhuo] debug
	/*
	char dumpStr[100];
	sprintf(dumpStr, "Core %d finish store", gproc->getId());
	dinst->dump(dumpStr);
	*/
	/////
	
  dinst->markExecuted();
	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

void WMMFUStore::performed(DInst *dinst) {
  dinst->markPerformed();
  executed(dinst);
}

bool WMMFUStore::preretire(DInst *dinst, bool flushing) {
	return true;
}

bool WMMFUStore::retire(DInst *dinst, bool flushing) {
	freeEntries++; // [sizhuo] release entry now
	return true;
}

