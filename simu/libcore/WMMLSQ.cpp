#include "WMMLSQ.h"
#include "GProcessor.h"
#include "MemRequest.h"
#include "SescConf.h"

WMMLSQ::WMMLSQ(GProcessor *gproc_, MemObj *DL1_)
	: gproc(gproc_)
	, DL1(DL1_)
	, maxLdNum(SescConf->getInt("cpusimu", "maxLoads", gproc->getId()))
	, maxStNum(SescConf->getInt("cpusimu", "maxStores", gproc->getId()))
	, freeLdNum(maxLdNum)
	, freeStNum(maxStNum)
	, ldldForwardDelay(SescConf->getInt("cpusimu", "ldldForwardDelay", gproc->getId()))
	, stldForwardDelay(SescConf->getInt("cpusimu", "stldForwardDelay", gproc->getId()))
	, specLSQEntryPool(maxLdNum + maxStNum, "WMMLSQ_specLSQEntryPool")
	, comSQEntryPool(maxStNum, "WMMLSQ_comSQEntryPool")
	, ldExPort(0)
	, nLdKillByLd("P(%d)_WMMLSQ_nLdKillByLd", gproc->getId())
	, nLdKillBySt("P(%d)_WMMLSQ_nLdKillBySt", gproc->getId())
	, nLdReExByLd("P(%d)_WMMLSQ_nLdReExByLd", gproc->getId())
	, nLdReExBySt("P(%d)_WMMLSQ_nLdReExBySt", gproc->getId())
	, nStLdForward("P(%d)_WMMLSQ_nStLdForward", gproc->getId())
	, nLdLdForward("P(%d)_WMMLSQ_nLdLdForward", gproc->getId())
{

	SescConf->isInt("cpusimu", "maxLoads", gproc->getId());
	SescConf->isInt("cpusimu", "maxStores", gproc->getId());
	SescConf->isInt("cpusimu", "ldldForwardDelay", gproc->getId());
	SescConf->isInt("cpusimu", "stldForwardDelay", gproc->getId());
	SescConf->isBetween("cpusimu", "maxLoads", 1, 256, gproc->getId());
	SescConf->isBetween("cpusimu", "maxStores", 1, 256, gproc->getId());
	SescConf->isBetween("cpusimu", "ldldForwardDelay", 1, 5, gproc->getId());
	SescConf->isBetween("cpusimu", "stldForwardDelay", 1, 5, gproc->getId());

	char portName[100];
	sprintf(portName, "P(%d)_WMMLSQ_LdExPort", gproc->getId());
	ldExPort = PortGeneric::create(portName, 1, 1);
}

StallCause WMMLSQ::addEntry(DInst *dinst) {
	I(dinst);
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
		const Instruction *const killIns = killDInst->getInst();
		// [sizhuo] we hit a poisoned inst, we can stop here
		if(killDInst->isPoisoned()) {
			break;
		}
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
					// TODO: add to store set
					killEn->needReEx = true;
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
					// [sizhuo] TODO: this load must be killed
					// TODO: add to store set
					//gproc->replay(en->dinst);
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

	// [sizhuo] TODO: check store set dependency here, for both load & store
	if(ins->isLoad()) {
		// [sizhuo] send load to execution
		scheduleLdEx(dinst);
	} else if(ins->isStore()) {
		// [sizhuo] store is done, change state, inform resource
		issueEn->state = Done;
		dinst->getClusterResource()->executed(dinst);
	} else {
		I(0);
	}
}

void WMMLSQ::scheduleLdEx(DInst *dinst) {
	I(dinst->getInst()->isLoad());
	ldExecuteCB::scheduleAbs(ldExPort->nextSlot(dinst->getStatsFlag()), this, dinst);
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
		I(!olderDInst->isPoisoned());
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
	dinst->getClusterResource()->executed(dinst);
	// [sizhuo] TODO: resolve store set dependency
}

void WMMLSQ::retire(DInst *dinst) {
	I(dinst);
	const Time_t id = dinst->getID();
	const AddrType addr = dinst->getAddr();
	const Instruction *const ins = dinst->getInst();
	I(ins->isLoad() || ins->isStore() || ins->isRecFence());

	// [sizhuo] find the entry in spec LSQ
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
		en->doStats = dinst->getStatsFlag();
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
			MemRequest::sendReqWrite(DL1, dinst->getStatsFlag(), addr, stCommitedCB::create(this, id));
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
	specLSQEntryPool.in(retireEn);
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
			MemRequest::sendReqWrite(DL1, en->doStats, addr, stCommitedCB::create(this, iter->first));
			break;
		}
	}

	// [sizhuo] recycle entry
	comSQEntryPool.in(comEn);
}
