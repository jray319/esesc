#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "SescConf.h"
#include "DInst.h"

SCTSOLSQ::SCTSOLSQ(GProcessor *gproc_, bool sc)
	: MTLSQ(gproc_)
	, isSC(sc)
	, lastComStID(DInst::invalidID)
{
	MSG("INFO: create P(%d)_SCTSOLSQ, isSC = %d", gproc->getId(), isSC);
}

StallCause SCTSOLSQ::addEntry(DInst *dinst) {
	I(dinst);
	I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
	I(freeLdNum >= 0);
	I(freeStNum >= 0);
	I(freeLdNum <= maxLdNum);
	I(freeStNum <= maxStNum);
	const Instruction *const ins = dinst->getInst();
	I(ins->isLoad() || ins->isStore() || ins->isRecFence());

	// [sizhuo] reconcile acts as NOP, just return
	if(ins->isRecFence()) {
		return NoStall;
	}

	// [sizhuo] load/store: check free entry & reduce free count
	if(ins->isLoad()) {
		if(freeLdNum == 0) {
			return OutsLoadsStall;
		}
		freeLdNum--;
		I(freeLdNum >= 0);
	} else if(ins->isStore()) {
		if(freeStNum == 0) {
			return OutsStoresStall;
		}
		freeStNum--;
		I(freeStNum >= 0);
	}
	return NoStall;
}

void SCTSOLSQ::issue(DInst *dinst) {
	I(dinst);
	const Instruction *const ins = dinst->getInst();
	const Time_t id = dinst->getID();
	const AddrType addr = dinst->getAddr();
	I(ins->isLoad() || ins->isStore());

	// [sizhuo] catch poisoned inst
	if(dinst->isPoisoned()) {
		// [sizhuo] mark executed without inserting to LSQ
		I(!dinst->isExecuted());
		dinst->markExecuted();
		return;
	}

	// [sizhuo] create new entry
	SpecLSQEntry *issueEn = specLSQEntryPool.out();
	issueEn->clear();
	issueEn->dinst = dinst;
	// [sizhuo] insert to spec LSQ
	std::pair<SpecLSQ::iterator, bool> insertRes = specLSQ.insert(std::make_pair<Time_t, SpecLSQEntry*>(id, issueEn));
	I(insertRes.second);
	SpecLSQ::iterator issueIter = insertRes.first;
	I(issueIter != specLSQ.end());

	// [sizhuo] store: search LSQ to kill eager loads
	if(ins->isStore()) {
		SpecLSQ::iterator iter = issueIter;
		iter++;
		for(; iter != specLSQ.end(); iter++) {
			I(iter->first > id);
			SpecLSQEntry *killEn = iter->second;
			I(killEn);
			DInst *killDInst = killEn->dinst;
			I(killDInst);
			I(!killDInst->isPoisoned());
			const Instruction *const killIns = killDInst->getInst();
			const bool doStats = killDInst->getStatsFlag();
			I(killIns->isLoad() || killIns->isStore());
			// [sizhuo] search inst to same ALIGNED address that reads across this store
			// XXX: since load won't kill load in SC/TSO, we won't stop search due to load
			// XXX: for store, generally (when bypass & kill has different granularity), we should not stop
			if(getMemOrdAlignAddr(killDInst->getAddr()) == getMemOrdAlignAddr(addr) && killIns->isLoad() && killEn->ldSrcID < id) {
				if(killEn->state == Exe) {
					// [sizhuo] we don't need to kill it, just let it re-execute
					// we can't train store set here, because many loads may be re-ex
					// so in WMM impl, we also only add to store set when ROB flush
					killEn->needReEx = true;
					// [sizhuo] stats
					nLdReExBySt.inc(doStats);
				} else if(killEn->state == Done) {
					// [sizhuo] this load must be killed & set replay reason
					// NOTE: 1 inst may be killed multiple times, the last kill wins
					killDInst->setReplayReason(DInst::Store);
					gproc->replay(killDInst);
					// [sizhuo] add to store set
					mtStoreSet->memDepViolate(dinst, killDInst);
					// [sizhuo] stats
					nLdKillBySt.inc(doStats);
					// [sizhuo] ROB will flush, we can stop
					break;
				}
			}
		}
	}

	if(ins->isLoad()) {
		// [sizhuo] load: send to execution
		scheduleLdEx(dinst);
	} else if(ins->isStore()) {
		// [sizhuo] change state to Done, inform resource
		issueEn->state = Done;
		// [sizhuo] call immediately, easy for flushing
		dinst->getClusterResource()->executed(dinst);
	} else {
		I(0);
	}
}

void SCTSOLSQ::ldExecute(DInst *dinst) {
	I(dinst);
	I(dinst->getInst()->isLoad());
	const AddrType addr = dinst->getAddr();
	const Time_t id = dinst->getID();
	const bool doStats = dinst->getStatsFlag();

	// [sizhuo] get execute entry
	SpecLSQ::iterator exIter = specLSQ.find(id);
	I(exIter != specLSQ.end());
	I(exIter->first == id);
	SpecLSQEntry *exEn = exIter->second;
	I(exEn);
	I(exEn->dinst == dinst);
	I(exEn->state == Wait);

	// [sizhuo] catch poisoned inst
	if(dinst->isPoisoned()) {
		removePoisonedEntry(exIter);
		return;
	}

	// [sizhuo] search older inst (with lower ID) in specLSQ for bypass or stall
	// XXX: decrement iterator smaller than begin() results in undefined behavior
	SpecLSQ::iterator iter = exIter;
	while(iter != specLSQ.begin()) {
		iter--;
		I(iter != specLSQ.end());
		SpecLSQEntry *olderEn = iter->second;
		I(olderEn);
		DInst *olderDInst = olderEn->dinst;
		I(olderDInst);
		const Instruction *const olderIns = olderDInst->getInst();
		I(!olderDInst->isPoisoned()); // [sizhuo] can't be poisoned
		I(iter->first < id);
		// [sizhuo] we hit reconcile fence, we should should stall until it retires
		if(olderIns->isRecFence()) {
			// [sizhuo] add event to pendRetireQ
			(olderEn->pendRetireQ).push(scheduleLdExCB::create(this, dinst));
			return;
		}
		// [sizhuo] we find inst to same ALIGNED address for bypass
		if(getMemOrdAlignAddr(olderDInst->getAddr()) == getMemOrdAlignAddr(addr)) {
			if(olderIns->isLoad() && olderEn->state == Done) {
				// [sizhuo] we can bypass from this executed load, mark as executing
				exEn->ldSrcID = olderEn->ldSrcID; // [sizhuo] same load src ID
				exEn->state = Exe;
				incrExLdNum(); // [sizhuo] increment executing ld num
				// [sizhuo] finish the load after forwarding delay
				ldDoneCB::schedule(ldldForwardDelay, this, dinst);
				// [sizhuo] stats
				nLdLdForward.inc(doStats);
				return;
			} else if(olderIns->isStore()) {
				// [sizhuo] we can bypass from this executed store
				// mark the entry as executing
				exEn->ldSrcID = olderDInst->getID(); // [sizhuo] record this store as load src
				exEn->state = Exe;
				incrExLdNum(); // [sizhuo] increment executing ld num
				// [sizhuo] finish the load after forwarding delay
				ldDoneCB::schedule(stldForwardDelay, this, dinst);
				// [sizhuo] stats
				nStLdForward.inc(doStats);
				return;
			}
		}
	}

	// [sizhuo] search commited store queue for bypass (start from youngest, i.e. largest ID)
	for(ComSQ::reverse_iterator rIter = comSQ.rbegin(); rIter != comSQ.rend(); rIter++) {
		ComSQEntry *en = rIter->second;
		I(en);
		I(rIter->first < id);
		// [sizhuo] XXX: bypass from same ALIGNED address
		if(getMemOrdAlignAddr(en->addr) == getMemOrdAlignAddr(addr)) {
			// [sizhuo] bypass from commited store, set ld as executing
			exEn->ldSrcID = rIter->first; // [sizhuo] record this store as load src
			exEn->state = Exe;
			incrExLdNum(); // [sizhuo] increment executing ld num
			// [sizhuo] finish load after forwarding delay
			ldDoneCB::schedule(stldForwardDelay, this, dinst);
			// [sizhuo] stats
			nStLdForward.inc(doStats);
			return;
		}
	}

	// [sizhuo] now we need to truly send load to memory hierarchy
	exEn->state = Exe;
	incrExLdNum(); // [sizhuo] increment executing ld num
	exEn->ldSrcID = DInst::invalidID; // [sizhuo] load src is memory
	MemRequest::sendReqRead(DL1, dinst->getStatsFlag(), addr, ldDoneCB::create(this, dinst));
}

bool SCTSOLSQ::retire(DInst *dinst) {
	I(dinst);
	I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
	const Time_t id = dinst->getID();
	const AddrType addr = dinst->getAddr();
	const Instruction *const ins = dinst->getInst();
	const bool doStats = dinst->getStatsFlag();
	I(ins->isLoad() || ins->isStore() || ins->isRecFence() || ins->isComFence());

	// [sizhuo] for reconcile fence, just return
	if(ins->isRecFence()) {
		return true;
	}

	// [sizhuo] for commit fence, SC TSO has different behavior
	if(ins->isComFence()) {
		if(isSC) {
			// [sizhuo] SC directly retires commit fence
			return true;
		} else {
			// [sizhuo] TSO has to wait comSQ empty
			return comSQ.empty();
		}
	}
	
	// [sizhuo] for SC, load can retire only when comSQ empty
	if(isSC && ins->isLoad() && !comSQ.empty()) {
		return false;
	}

	I(ins->isStore() || (ins->isLoad() && (!isSC || comSQ.empty())));

	// [sizhou] load/store must be in specLSQ, find the entry
	SpecLSQ::iterator retireIter = specLSQ.find(id);
	I(retireIter == specLSQ.begin());
	I(retireIter != specLSQ.end());
	I(retireIter->first == dinst->getID());
	SpecLSQEntry *retireEn = retireIter->second;
	I(retireEn);
	I(retireEn->dinst == dinst);
	I(retireEn->state == Done);
	// [sizhuo] pend ex/retire Q must be empty
	I((retireEn->pendExQ).empty());
	I((retireEn->pendRetireQ).empty());

	// [sizhuo] retire from spec LSQ
	specLSQ.erase(retireIter);
	// [sizhuo] only free load entry
	if(ins->isLoad()) {
		freeLdNum++;
		I(freeLdNum > 0);
		I(freeLdNum <= maxLdNum);
	}
	
	// [sizhuo] actions when retiring
	if(ins->isStore()) {
		// [sizhuo] retire store to commited store Q
		ComSQEntry *en = comSQEntryPool.out();
		en->clear();
		en->addr = addr;
		en->doStats = doStats;
		std::pair<ComSQ::iterator, bool> insertRes = comSQ.insert(std::make_pair<Time_t, ComSQEntry*>(id, en));
		I(insertRes.second);
		ComSQ::iterator comIter = insertRes.first;
		I(comIter != comSQ.end());
		// [sizhuo] if comSQ doesn't have older store, send this one to memory
		if(comSQ.size() == 1) {
			I(comIter == comSQ.begin());
			stToMemCB::scheduleAbs(stToMemPort->nextSlot(doStats), this, addr, id, doStats);
		}
	} else if(ins->isLoad()) {
		// decrement done ld num
		doneLdNum--;
		I(doneLdNum >= 0);
	}

	// [sizhuo] recycle spec LSQ entry
	I((retireEn->pendRetireQ).empty() && (retireEn->pendExQ).empty());
	specLSQEntryPool.in(retireEn);

	return true;
}

void SCTSOLSQ::stCommited(Time_t id) {
	// [sizhuo] release store entry
	freeStNum++;
	I(freeStNum > 0);
	I(freeStNum <= maxStNum);
	// [sizhuo] delete the store from ComSQ (must be the oldest entry)
	ComSQ::iterator comIter = comSQ.begin();
	I(comIter != comSQ.end());
	I(comIter->first == id);
	ComSQEntry *comEn = comIter->second;
	I(comEn);
	comSQ.erase(comIter);

	// [sizhuo] update last commited store ID
	lastComStID = id;

	// [sizhuo] send the current oldest entry to memory
	if(!comSQ.empty()) {
		ComSQ::iterator iter = comSQ.begin();
		I(iter->first > id);
		ComSQEntry *en = iter->second;
		I(en);
		stToMemCB::scheduleAbs(stToMemPort->nextSlot(en->doStats), this, en->addr, iter->first, en->doStats);
	}

	// [sizhuo] recycle entry
	comSQEntryPool.in(comEn);
}

void SCTSOLSQ::cacheInv(AddrType lineAddr, uint32_t shift) {
	// [sizhuo] search LSQ to kill eager loads on same CACHE LINE addr
	for(SpecLSQ::iterator iter = specLSQ.begin(); iter != specLSQ.end(); iter++) {
		SpecLSQEntry *killEn = iter->second;
		I(killEn);
		DInst *killDInst = killEn->dinst;
		I(killDInst);
		const Instruction *const killIns = killDInst->getInst();
		const bool doStats = killDInst->getStatsFlag();
		I(killIns->isLoad() || killIns->isStore());
		// [sizhuo] if we hit a poisoned inst, we can stop
		if(killDInst->isPoisoned()) {
			break;
		}
		// [sizhuo] search executed load to same CACHE LINE address
		// XXX: we use <= in comparison of load src ID
		if((killDInst->getAddr() >> shift) == lineAddr && killIns->isLoad() && killEn->state == Done && killEn->ldSrcID <= lastComStID) {
			// [sizhuo] this load must be killed & set replay reason
			killDInst->setReplayReason(DInst::CacheInv);
			gproc->replay(killDInst);
			// [sizhuo] stats
			nLdKillByInv.inc(doStats);
			// [sizhuo] ROB will flush, we can stop
			break;
		}
		// [sizhuo] we don't re-ex load in execution
		// because invalidation arrives earlier than load resp
		// load must be a miss in L1$
	}
}

