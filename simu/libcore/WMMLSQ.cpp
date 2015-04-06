#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "SescConf.h"
#include "DInst.h"

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
	const bool doStats = dinst->getStatsFlag();
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
		// [sizhuo] we hit a reconcile fence we can stop
		if(killIns->isRecFence()) {
			break;
		}
		// [sizhuo] search inst to same address
		if(killDInst->getAddr() == addr) {
			if(killIns->isStore()) {
				// [sizhuo] remaining load either bypass from this load or get killed
				// stop here
				break;
			} else if(killIns->isLoad()) {
				if(killEn->state == Wait) {
					// [sizhuo] this load is good
					// remaining younger loads are either stalled by this one, or killed by it
					// so we can stop here
					break;
				} else if(killEn->state == Exe) {
					// [sizhuo] we don't need to kill it, just let it re-execute
					killEn->needReEx = true;
					// [sizhuo] add to store set
					mtStoreSet->memDepViolate(dinst, killDInst);
					// [sizhuo] stats
					if(ins->isStore()) {
						nLdReExBySt.inc(doStats);
					} else {
						nLdReExByLd.inc(doStats);
					}
					// [sizhuo] remaining younger loads are either stalled by this one, or killed by it
					// so we can stop here
					break;
				} else if(killEn->state == Done) {
					// [sizhuo] this load must be killed
					gproc->replay(killDInst);
					// [sizhuo] add to store set
					mtStoreSet->memDepViolate(dinst, killDInst);
					// [sizhuo] stats
					if(ins->isStore()) {
						nLdKillBySt.inc(doStats);
					} else {
						nLdKillByLd.inc(doStats);
					}
					break;
				} else {
					I(0);
				}
			} else {
				I(0);
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
		// [sizhuo] we find inst to same address
		if(olderDInst->getAddr() == addr) {
			if(olderIns->isLoad()) {
				if(olderEn->state == Wait || olderEn->state == Exe) {
					// [sizhuo] load not finished execution, but address has resolved
					// we should stall until this inst has finished execution
					(olderEn->pendExQ).push(scheduleLdExCB::create(this, dinst));
					return;
				} else if(olderEn->state == Done) {
					// [sizhuo] we can bypass from this executed load, mark as executing
					exEn->state = Exe;
					// [sizhuo] finish the load after forwarding delay
					ldDoneCB::schedule(ldldForwardDelay, this, dinst);
					// [sizhuo] stats
					nLdLdForward.inc(doStats);
					return;
				} else {
					I(0);
				}
			} else if(olderIns->isStore()) {
				// [sizhuo] we can bypass from this executed store
				// mark the entry as executing
				exEn->state = Exe;
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
		if(en->addr == addr) {
			// [sizhuo] bypass from commited store, set ld as executing
			exEn->state = Exe;
			// [sizhuo] finish load after forwarding delay
			ldDoneCB::schedule(stldForwardDelay, this, dinst);
			// [sizhuo] stats
			nStLdForward.inc(doStats);
			return;
		}
	}

	// [sizhuo] now we need to truly send load to memory hierarchy
	exEn->state = Exe;
	MemRequest::sendReqRead(DL1, dinst->getStatsFlag(), addr, ldDoneCB::create(this, dinst));
}

void WMMLSQ::ldDone(DInst *dinst) {
	I(dinst);
	I(dinst->getInst()->isLoad());

	// [sizhuo] get done entry
	SpecLSQ::iterator doneIter = specLSQ.find(dinst->getID());
	I(doneIter != specLSQ.end());
	I(doneIter->first == dinst->getID());
	SpecLSQEntry *doneEn = doneIter->second;
	I(doneEn);
	I(doneEn->dinst == dinst);
	I(doneEn->state == Exe);

	// [sizhuo] catch poisoned inst
	if(dinst->isPoisoned()) {
		removePoisonedEntry(doneIter);
		return;
	}

	// [sizhuo] check re-execute bit
	if(doneEn->needReEx) {
		// [sizhuo] re-schedule this entry for execution
		doneEn->needReEx = false;
		doneEn->state = Wait;
		scheduleLdEx(dinst);
		return;
	}

	// [sizhuo] this load is truly done, change state
	doneEn->state = Done;
	// [sizhuo] call pending events next cycle
	while(!(doneEn->pendExQ).empty()) {
		CallbackBase *cb = (doneEn->pendExQ).front();
		(doneEn->pendExQ).pop();
		cb->schedule(1);
	}
	// [sizhuo] inform resource that this load is done at this cycle
	// calling immediately is easy for flushing
	dinst->getClusterResource()->executed(dinst);
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
		// [sizhuo] if comSQ doesn't have older store to same addr, send this one to memory
		bool noOlderSt = true;
		while(comIter != comSQ.begin()) {
			comIter--;
			I(comIter != comSQ.end());
			I(comIter->second);
			I(comIter->first < id);
			if(comIter->second->addr == addr) {
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
	}

	// [sizhuo] recycle spec LSQ entry
	I((retireEn->pendRetireQ).empty() && (retireEn->pendExQ).empty());
	specLSQEntryPool.in(retireEn);

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

	// [sizhuo] send the current oldest entry of this addr to memory
	const AddrType addr = comEn->addr;
	for(ComSQ::iterator iter = comSQ.begin(); iter != comSQ.end(); iter++) {
		ComSQEntry *en = iter->second;
		I(en);
		if(en->addr == addr) {
			I(iter->first > id);
			stToMemCB::scheduleAbs(stToMemPort->nextSlot(en->doStats), this, addr, iter->first, en->doStats);
			break;
		}
	}

	// [sizhuo] recycle entry
	comSQEntryPool.in(comEn);
}

void WMMLSQ::reset() {
	// [sizhuo] remove all reconcile & store & executed load
	while(true) {
		SpecLSQ::iterator rmIter = specLSQ.begin();
		// [sizhuo] search through specLSQ for reconcile & store & executed load
		// because these entries are only invoked by retire(), which will not be called in flushing mode
		for(; rmIter != specLSQ.end(); rmIter++) {
			SpecLSQEntry *rmEn = rmIter->second;
			I(rmEn);
			I(rmEn->dinst);
			const Instruction *const rmIns = rmEn->dinst->getInst();
			I(rmIns);
			if(rmIns->isRecFence() || rmIns->isStore() || (rmIns->isLoad() && rmEn->state == Done)) {
				break;
			}
		}
		if(rmIter != specLSQ.end()) {
			// [sizhuo] find a reconcile / store to remove
			removePoisonedEntry(rmIter);
		} else {
			// [sizhuo] no more reconcile or store, stop
			break;
		}
	}
	// [sizhuo] recover free entry num
	freeLdNum = maxLdNum;
	freeStNum = maxStNum - comSQ.size();
	I(freeStNum >= 0);
}

bool WMMLSQ::isReset() {
	return specLSQ.empty() && freeLdNum == maxLdNum && freeStNum == (maxStNum - comSQ.size());
}
