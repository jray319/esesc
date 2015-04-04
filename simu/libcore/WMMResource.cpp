
#include <limits.h>
#include "WMMResource.h"

#include "Cluster.h"
#include "DInst.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "GProcessor.h"
#include "WMMProcessor.h"
#include "MemRequest.h"
#include "MemObj.h"
#include "Port.h"

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
WMMLSResource::WMMLSResource(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, int32_t id)
	: Resource(cls, aGen, l)
	, lsq(q)
	, storeset(NULL)
{
  I(SescConf->getBool("cpusimu", "enableDcache", id));
	I(q);
}

// [sizhuo] WMMFULoad class: load unit
WMMFULoad::WMMFULoad(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, int32_t id)
	: WMMLSResource(cls, aGen, l, q, id)
{
}

StallCause WMMFULoad::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	// [sizhuo] reserve entry in LSQ, reconcile fence is truly added
	return lsq->addEntry(dinst);
}

void WMMFULoad::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

	cluster->executing(dinst); // [sizhuo] just change wake up time
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	const Instruction *const ins = dinst->getInst();
	if(ins->isRecFence()) {
		// [sizhuo] reconcile: mark executed
		executedCB::scheduleAbs(when, this, dinst);
	} else {
		I(ins->isLoad());
		// [sizhuo] load: schedule call back to issue to LSQ
		WMMLSQ::issueCB::scheduleAbs(when, lsq, dinst);
	}
}

void WMMFULoad::performed(DInst *dinst) {
	I(0); // [sizhuo] obsolete
}

void WMMFULoad::executed(DInst *dinst) {
  dinst->markExecuted();
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		return;
	}

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

	// [sizhuo] retire load/reconcile from LSQ
	lsq->retire(dinst);
	return true;
}

// [sizhuo] WMMFUStore class: store unit
WMMFUStore::WMMFUStore(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, MemObj *dcache, int32_t id)
	: WMMLSResource(cls, aGen, l, q, id)
	, DL1(dcache)
{
	I(DL1);
}

StallCause WMMFUStore::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	const Instruction *const ins = dinst->getInst();
	// [sizhuo] for store addr & commit fence, no need to add to LSQ
  if (ins->isStoreAddress()) {
    return NoStall;
	}
	if(ins->isComFence()) {
		return NoStall;
	}

	// [sizhuo] store needs to be added to LSQ
	I(ins->isStore());
  return lsq->addEntry(dinst);
}

void WMMFUStore::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		dinst->markExecuted();
		return;
	}

  cluster->executing(dinst);
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

	const Instruction *const ins = dinst->getInst();
	if(ins->isStoreAddress() || ins->isComFence()) {
		// [sizhuo] store addr or commit: mark as executed
		executedCB::scheduleAbs(when, this, dinst);
	} else {
		// [sizhuo] store: schedule callback to issue to LSQ
		I(ins->isStore());
		WMMLSQ::issueCB::scheduleAbs(when, lsq, dinst);
	}
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

	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

void WMMFUStore::performed(DInst *dinst) {
	I(0); // [sizhuo] obsolete
}

bool WMMFUStore::preretire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	return true;
}

bool WMMFUStore::retire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	const Instruction *const ins = dinst->getInst();
	if(ins->isStoreAddress()) {
		// [sizhuo] store addr: just retire
		return true;
	} else if(ins->isComFence()) {
		// [sizhuo] commit: retire only when commited SQ empty
		return lsq->isComSQEmpty();
	} else {
		// [sizhuo] store: retire store to commited SQ
		I(ins->isStore());
		lsq->retire(dinst);
		return true;
	}
}

