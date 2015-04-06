
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
#include "MTStoreSet.h"

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
		I(!dinst->isExecuted());
		dinst->markExecuted();
		return;
	}

  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}

void WMMFURALU::executed(DInst *dinst) {
	I(!dinst->isExecuted());
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
WMMLSResource::WMMLSResource(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, MTStoreSet *ss, int32_t id)
	: Resource(cls, aGen, l)
	, lsq(q)
	, mtStoreSet(ss)
{
  I(SescConf->getBool("cpusimu", "enableDcache", id));
	I(q);
	I(ss);
}

// [sizhuo] WMMFULoad class: load unit
WMMFULoad::WMMFULoad(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, MTStoreSet *ss, int32_t id)
	: WMMLSResource(cls, aGen, l, q, ss, id)
{
}

StallCause WMMFULoad::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());
	I(dinst->getInst()->isLoad() || dinst->getInst()->isRecFence());

	// [sizhuo] reserve entry in LSQ, reconcile fence is truly added
	StallCause sc = lsq->addEntry(dinst);
	if(sc != NoStall) {
		return sc;
	}
	// [sizhuo] add to store set & create memory dependency
	if(dinst->getInst()->isLoad()) {
		mtStoreSet->insert(dinst);
	}
	return NoStall;
}

void WMMFULoad::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		if(!dinst->isExecuted()) {
			dinst->markExecuted();
		}
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
	if(!dinst->isExecuted()) {
		// [sizhuo] if reconcile fence is killed,
		// it may already be marked as executed
		dinst->markExecuted();
	}
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		// [sizhuo] this can only be reconcile fence
		// and its removal from LSQ is already done in WMMLSQ::reset
		// no need to remove from store set
		I(dinst->getInst()->isRecFence());
		return;
	}

	// [sizhuo] remove from store set
	if(dinst->getInst()->isLoad()) {
		mtStoreSet->remove(dinst);
	}
	// [sizhuo] wake up inst with data dependency on dinst
  cluster->executed(dinst);
}

bool WMMFULoad::preretire(DInst *dinst, bool flushing) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());
	I(0); // [sizhuo] obsolete
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
WMMFUStore::WMMFUStore(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, LSQ *q, MTStoreSet *ss, MemObj *dcache, int32_t id)
	: WMMLSResource(cls, aGen, l, q, ss, id)
	, DL1(dcache)
{
	I(DL1);
}

StallCause WMMFUStore::canIssue(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

	const Instruction *const ins = dinst->getInst();
	I(ins->isStore() || ins->isStoreAddress() || ins->isComFence());
	// [sizhuo] for store addr & commit fence, no need to add to LSQ
  if (ins->isStoreAddress()) {
    return NoStall;
	}
	if(ins->isComFence()) {
		return NoStall;
	}

	// [sizhuo] store needs to be added to LSQ
	I(ins->isStore());
  StallCause sc = lsq->addEntry(dinst);
	if(sc != NoStall) {
		return sc;
	}
	// [sizhuo] add to store set & create memory dependency
	if(dinst->getInst()->isStore()) {
		mtStoreSet->insert(dinst);
	}
	return NoStall;
}

void WMMFUStore::executing(DInst *dinst) {
	// [sizhuo] detect poisoned inst & mark as executed
	if(dinst->isPoisoned()) {
		I(!dinst->isExecuted());
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
	I(!dinst->isExecuted());
  dinst->markExecuted();
	
	// [sizhuo] detect poisoned inst & just return
	if(dinst->isPoisoned()) {
		// [sizhuo] this can only be commit fence or store addr
		I(dinst->getInst()->isComFence() || dinst->getInst()->isStoreAddress());
		return;
	}

	if(dinst->getInst()->isStoreAddress()) {
		// [sizhuo] do prefetch
		I(!DL1->isBusy(dinst->getAddr())); // [sizhuo] cache always available
		MemRequest::sendReqWritePrefetch(DL1, dinst->getStatsFlag(), dinst->getAddr());
	} else if(dinst->getInst()->isStore()) {
		// [sizhuo] remove from store set
		mtStoreSet->remove(dinst);
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

