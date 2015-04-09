#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "SescConf.h"
#include "DInst.h"

WMMLSQ::WMMLSQ(GProcessor *gproc_)
	: MTLSQ(gproc_) 
{
	MSG("INFO: create P(%d)_WMMLSQ", gproc->getId());
}

StallCause WMMLSQ::addEntry(DInst *dinst) {
	I(dinst);
	I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
	I(freeLdNum >= 0);
	I(freeStNum >= 0);
	I(freeLdNum <= maxLdNum);
	I(freeStNum <= maxStNum);
	const Instruction *const ins = dinst->getInst();
	I(ins->isLoad() || ins->isStore() || ins->isRecFence());

	// [sizhuo] check free entry & reduce free count
	if(ins->isLoad() || ins->isRecFence()) {
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

	// [sizhuo] immediately insert reconcile fence into LSQ
	// to make sure no younger load misses it
	if(ins->isRecFence()) {
		SpecLSQEntry *en = specLSQEntryPool.out();
		I(en);
		en->clear();
		en->dinst = dinst;
		en->state = Done;
		specLSQ.insert(std::make_pair<Time_t, SpecLSQEntry*>(dinst->getID(), en));
	}

	return NoStall;
}

void WMMLSQ::issue(DInst *dinst) {
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

	// [sizhuo] search younger entry (with higer ID) to find eager load to kill
	// TODO: if we are clever enough, load can bypass from younger load instead of killing
	SpecLSQ::iterator iter = issueIter;
	iter++;
	for(; iter != specLSQ.end(); iter++) {
		I(iter->first > id);
		SpecLSQEntry *killEn = iter->second;
		I(killEn);
		DInst *killDInst = killEn->dinst;
		I(killDInst);
		I(!killDInst->isPoisoned()); // [sizhuo] can't be poisoned
		const Instruction *const killIns = killDInst->getInst();
		const bool doStats = killDInst->getStatsFlag();
		// [sizhuo] we hit a reconcile fence we can stop
		if(killIns->isRecFence()) {
			break;
		}
		// XXX: [sizhuo] search load to same ALIGNED address that reads across the issuing inst
		// XXX: we decide kill/re-ex based on load source ID
		if(getMemOrdAlignAddr(killDInst->getAddr()) == getMemOrdAlignAddr(addr)) {
			if(killIns->isLoad()) {
				if(killEn->state == Wait) {
					// [sizhuo] younger loads on same aligned addr cannot read across this one
					// they are either stalled or killed, so stop here
					break;
				} else if(killEn->state == Exe) {
					if(killEn->ldSrcID < id) {
						// [sizhuo] we don't need to kill it, just let it re-execute
						killEn->needReEx = true;
						// [sizhuo] don't add to store set
						// because SC impl only adds when ROB flush
						// [sizhuo] stats
						if(ins->isStore()) {
							nLdReExBySt.inc(doStats);
						} else {
							nLdReExByLd.inc(doStats);
						}
					}
					// [sizhuo] younger loads on same aligned addr cannot read across this one
					// they are either stalled or killed, so stop here
					break;
				} else if(killEn->state == Done) {
					if(killEn->ldSrcID < id) {
						// [sizhuo] this load must be killed & set replay reason
						// NOTE: 1 inst may be killed multiple times, the last kill wins
						killDInst->setReplayReason(ins->isStore() ? DInst::Store : DInst::Load);
						gproc->replay(killDInst);
						// [sizhuo] add to store set
						mtStoreSet->memDepViolate(dinst, killDInst);
						// [sizhuo] stats
						if(ins->isStore()) {
							nLdKillBySt.inc(doStats);
						} else {
							nLdKillByLd.inc(doStats);
						}
						break; // [sizhuo] ROB will flush, we can stop
					} else {
						// [sizhuo] if bypass & kill are at same alignment
						// then we can stop here
						// but generally we should continue the search
					}
				} else {
					I(0);
				}
			} else {
				I(killIns->isStore());
				// [sizhuo] if bypass & kill are at same alignment
				// then we can stop here
				// but generally we should continue the search
			}
		}
	}

	if(ins->isLoad()) {
		// [sizhuo] send load to execution
		scheduleLdEx(dinst);
	} else if(ins->isStore()) {
		// [sizhuo] store is done, change state, inform resource
		issueEn->state = Done;
		// [sizhuo] call immediately, easy for flushing
		dinst->getClusterResource()->executed(dinst);
	} else {
		I(0);
	}
}

void WMMLSQ::ldExecute(DInst *dinst) {
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
		// [sizhuo] we find inst to same ALIGNED address for bypass or stall
		if(getMemOrdAlignAddr(olderDInst->getAddr()) == getMemOrdAlignAddr(addr)) {
			if(olderIns->isLoad()) {
				if(olderEn->state == Done) {
					// [sizhuo] we can bypass from this executed load, mark as executing
					exEn->ldSrcID = olderEn->ldSrcID; // [sizhuo] same load src ID
					exEn->state = Exe;
					incrExLdNum(); // [sizhuo] increment executing ld num
					// [sizhuo] finish the load after forwarding delay
					ldDoneCB::schedule(ldldForwardDelay, this, dinst);
					// [sizhuo] stats
					nLdLdForward.inc(doStats);
					return;
				} else if(olderEn->state == Wait || olderEn->state == Exe) {
					// [sizhuo] stall on this older load which has not finished
					(olderEn->pendExQ).push(scheduleLdExCB::create(this, dinst));
					// [sizhuo] don't add to store set
					// because SC impl only add to store set when ROB flush
					// [sizhuo] stats
					nLdStallByLd.inc(doStats);
					return;
				} else {
					I(0);
				}
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
			} else {
				I(0);
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

bool WMMLSQ::retire(DInst *dinst) {
	I(dinst);
	I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
	const Time_t id = dinst->getID();
	const AddrType addr = dinst->getAddr();
	const Instruction *const ins = dinst->getInst();
	const bool doStats = dinst->getStatsFlag();
	I(ins->isLoad() || ins->isStore() || ins->isRecFence() || ins->isComFence());

	// [sizhuo] for commit fence, just check comSQ empty
	if(ins->isComFence()) {
		return comSQ.empty();
	}

	// [sizhuo] for other inst, they must be in spec LSQ
	// find the entry in spec LSQ
	SpecLSQ::iterator retireIter = specLSQ.find(id);
	I(retireIter == specLSQ.begin());
	I(retireIter != specLSQ.end());
	I(retireIter->first == dinst->getID());
	SpecLSQEntry *retireEn = retireIter->second;
	I(retireEn);
	I(retireEn->dinst == dinst);
	I(retireEn->state == Done);
	// [sizhuo] pend ex Q must be empty
	I((retireEn->pendExQ).empty());
	// [sizhuo] load/store: pend retire Q must be empty
	GI(dinst->getInst()->isLoad() || dinst->getInst()->isStore(), (retireEn->pendRetireQ).empty());

	// [sizhuo] retire from spec LSQ
	specLSQ.erase(retireIter);
	// [sizhuo] only free load entry (LD/RECONCILE)
	if(ins->isLoad() || ins->isRecFence()) {
		freeLdNum++;
		I(freeLdNum > 0);
		I(freeLdNum <= maxLdNum);
	}

	// [sizhuo] actions when retiring
	if(ins->isStore()) {
		// [sizhuo] store: insert to commited store Q
		ComSQEntry *en = comSQEntryPool.out();
		en->clear();
		en->addr = addr;
		en->doStats = doStats;
		std::pair<ComSQ::iterator, bool> insertRes = comSQ.insert(std::make_pair<Time_t, ComSQEntry*>(id, en));
		I(insertRes.second);
		ComSQ::iterator comIter = insertRes.first;
		I(comIter != comSQ.end());
		// [sizhuo] XXX: if comSQ doesn't have older store to same ALIGNED addr
		// send this one to memory
		bool noOlderSt = true;
		while(comIter != comSQ.begin()) {
			comIter--;
			I(comIter != comSQ.end());
			I(comIter->second);
			I(comIter->first < id);
			if(getMemOrdAlignAddr(comIter->second->addr) == getMemOrdAlignAddr(addr)) {
				noOlderSt = false;
				break;
			}
		}
		if(noOlderSt) {
			stToMemCB::scheduleAbs(stToMemPort->nextSlot(doStats), this, addr, id, doStats);
		}
	} else if(ins->isRecFence()) {
		// [sizhuo] reconcile: call events in pend retireQ next cycle
		while(!(retireEn->pendRetireQ).empty()) {
			CallbackBase *cb = (retireEn->pendRetireQ).front();
			(retireEn->pendRetireQ).pop();
			cb->schedule(1);
		}
	} else if(ins->isLoad()) {
		// decrement done ld num
		doneLdNum--;
		I(doneLdNum >= 0);
	}

	// [sizhuo] recycle spec LSQ entry
	I((retireEn->pendRetireQ).empty() && (retireEn->pendExQ).empty());
	specLSQEntryPool.in(retireEn);

	// [sizhuo] unalignment stats
	if(addr & ((0x01ULL << memOrdAlignShift) - 1)) {
		if(ins->isLoad()) {
			nUnalignLd.inc(doStats);
		} else if(ins->isStore()) {
			nUnalignSt.inc(doStats);
		}
	}

	// [sizhuo] retire success
	return true;
}

void WMMLSQ::stCommited(Time_t id) {
	// [sizhuo] release store entry
	freeStNum++;
	I(freeStNum > 0);
	I(freeStNum <= maxStNum);
	// [sizhuo] delete the store from ComSQ
	ComSQ::iterator comIter = comSQ.find(id);
	I(comIter != comSQ.end());
	ComSQEntry *comEn = comIter->second;
	I(comEn);
	comSQ.erase(comIter);

	// [sizhuo] send the current oldest entry of this ALIGNED addr to memory
	const AddrType addr = comEn->addr;
	for(ComSQ::iterator iter = comSQ.begin(); iter != comSQ.end(); iter++) {
		ComSQEntry *en = iter->second;
		I(en);
		if(getMemOrdAlignAddr(en->addr) == getMemOrdAlignAddr(addr)) {
			I(iter->first > id);
			stToMemCB::scheduleAbs(stToMemPort->nextSlot(en->doStats), this, addr, iter->first, en->doStats);
			break;
		}
	}

	// [sizhuo] recycle entry
	comSQEntryPool.in(comEn);
}

