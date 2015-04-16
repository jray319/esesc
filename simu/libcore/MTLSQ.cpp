#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "GMemorySystem.h"
#include "SescConf.h"
#include "DInst.h"

const uint32_t MTLSQ::memOrdAlignShift = 2; // [sizhuo] word aligned

MTLSQ* MTLSQ::create(GProcessor *gproc_) {
	SescConf->isCharPtr("cpusimu", "memModel", gproc_->getId());
	SescConf->isBool("cpusimu", "sctsoLdWait", gproc_->getId());
	const char *memModel = SescConf->getCharPtr("cpusimu", "memModel", gproc_->getId());
	const bool ldWait = SescConf->getBool("cpusimu", "sctsoLdWait", gproc_->getId());
	MTLSQ *ret = 0;

	if(!strcasecmp(memModel, "wmm")) {
		ret = new WMMLSQ(gproc_);
	} else if(!strcasecmp(memModel, "tso")) {
		ret = new SCTSOLSQ(gproc_, false, ldWait);
	} else if(!strcasecmp(memModel, "sc")) {
		ret = new SCTSOLSQ(gproc_, true, ldWait);
	} else {
		SescConf->notCorrect();
	}
	return ret;
}

MTLSQ::MTLSQ(GProcessor *gproc_)
	: gproc(gproc_)
	, mtStoreSet(gproc->getMTSS())
	, DL1(gproc->getMemorySystem()->getDL1())
	, maxLdNum(SescConf->getInt("cpusimu", "maxLoads", gproc->getId()))
	, maxStNum(SescConf->getInt("cpusimu", "maxStores", gproc->getId()))
	, freeLdNum(maxLdNum)
	, freeStNum(maxStNum)
	, ldldForwardDelay(SescConf->getInt("cpusimu", "ldldForwardDelay", gproc->getId()))
	, stldForwardDelay(SescConf->getInt("cpusimu", "stldForwardDelay", gproc->getId()))
	, specLSQEntryPool(maxLdNum + maxStNum, "MTLSQ_specLSQEntryPool")
	, comSQEntryPool(maxStNum, "MTLSQ_comSQEntryPool")
	, ldExPort(0)
	, stToMemPort(0)
	, nLdStallByLd("P(%d)_MTLSQ_nLdStallByLd", gproc->getId())
	, nLdStallByRec("P(%d)_MTLSQ_nLdStallByRec", gproc->getId())
	, nLdKillByLd("P(%d)_MTLSQ_nLdKillByLd", gproc->getId())
	, nLdKillBySt("P(%d)_MTLSQ_nLdKillBySt", gproc->getId())
	, nLdKillByInv("P(%d)_MTLSQ_nLdKillByInv", gproc->getId())
	, nLdReExByLd("P(%d)_MTLSQ_nLdReExByLd", gproc->getId())
	, nLdReExBySt("P(%d)_MTLSQ_nLdReExBySt", gproc->getId())
	, nLdReExByInv("P(%d)_MTLSQ_nLdReExByInv", gproc->getId())
	, nStLdForward("P(%d)_MTLSQ_nStLdForward", gproc->getId())
	, nLdLdForward("P(%d)_MTLSQ_nLdLdForward", gproc->getId())
	, nUnalignLd("P(%d)_MTLSQ_nUnalignLd", gproc->getId())
	, nUnalignSt("P(%d)_MTLSQ_nUnalignSt", gproc->getId())
	, exLdNum(0)
	, doneLdNum(0)
{
	SescConf->isInt("cpusimu", "maxLoads", gproc->getId());
	SescConf->isInt("cpusimu", "maxStores", gproc->getId());
	SescConf->isInt("cpusimu", "ldldForwardDelay", gproc->getId());
	SescConf->isInt("cpusimu", "stldForwardDelay", gproc->getId());
	SescConf->isBetween("cpusimu", "maxLoads", 1, 256, gproc->getId());
	SescConf->isBetween("cpusimu", "maxStores", 1, 1024, gproc->getId()); // [sizhuo] allow extremely large SQ
	SescConf->isBetween("cpusimu", "ldldForwardDelay", 1, 5, gproc->getId());
	SescConf->isBetween("cpusimu", "stldForwardDelay", 1, 5, gproc->getId());

	I(gproc);
	I(mtStoreSet);
	I(DL1);

	// [sizhuo] set LSQ field in L1 D$
	DL1->setMTLSQ(this);

	char portName[100];

	sprintf(portName, "P(%d)_MTLSQ_LdExPort", gproc->getId());
	ldExPort = PortGeneric::create(portName, 1, 1);
	I(ldExPort);

	sprintf(portName, "P(%d)_MTLSQ_StToMemPort", gproc->getId());
	stToMemPort = PortGeneric::create(portName, 1, 1);
	I(stToMemPort);
}

void MTLSQ::scheduleLdEx(DInst *dinst) {
	I(dinst->getInst()->isLoad());

	// [sizhuo] catch poisoned inst
	if(dinst->isPoisoned()) {
		removePoisonedInst(dinst);
		return;
	}

	ldExecuteCB::scheduleAbs(ldExPort->nextSlot(dinst->getStatsFlag()), this, dinst);
}

void MTLSQ::ldDone(DInst *dinst) {
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

	// [sizhuo] decrement number of executing loads
	exLdNum--;
	I(exLdNum >= 0);

	// [sizhuo] check re-execute bit
	if(doneEn->needReEx) {
		// [sizhuo] re-schedule this entry for execution
		doneEn->needReEx = false;
		doneEn->state = Wait;
		doneEn->ldSrcID = DInst::invalidID;
		scheduleLdEx(dinst);
		return;
	}

	// [sizhuo] this load is truly done, change state
	doneEn->state = Done;
	// [sizhuo] increment done load num
	doneLdNum++;
	I(exLdNum + doneLdNum <= maxLdNum);
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

void MTLSQ::removePoisonedEntry(SpecLSQ::iterator rmIter) {
	I(rmIter != specLSQ.end());
	// [sizhuo] get entry pointer
	SpecLSQEntry *rmEn = rmIter->second;
	I(rmEn);
	I(rmEn->dinst);
	I(rmEn->dinst->isPoisoned());
	// [sizhuo] mark inst as executed
	if(!rmEn->dinst->isExecuted()) {
		rmEn->dinst->markExecuted();
	}
	// [sizhuo] erase the entry
	specLSQ.erase(rmIter);
	// [sizhuo] call all events in pending Q
	while(!(rmEn->pendExQ).empty()) {
		CallbackBase *cb = (rmEn->pendExQ).front();
		(rmEn->pendExQ).pop();
		cb->call();
	}
	while(!(rmEn->pendRetireQ).empty()) {
		CallbackBase *cb = (rmEn->pendRetireQ).front();
		(rmEn->pendRetireQ).pop();
		cb->call();
	}
	// [sizhuo] recycle the entry
	I((rmEn->pendRetireQ).empty() && (rmEn->pendExQ).empty());
	specLSQEntryPool.in(rmEn);
}

void MTLSQ::removePoisonedInst(DInst *dinst) {
	I(dinst);
	I(dinst->isPoisoned());
	SpecLSQ::iterator rmIter = specLSQ.find(dinst->getID());
	I(rmIter->second);
	I(rmIter->second->dinst == dinst);
	removePoisonedEntry(rmIter);
}

void MTLSQ::reset() {
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
	// [sizhuo] recover stats vars
	exLdNum = 0;
	doneLdNum = 0;
}

bool MTLSQ::isReset() {
	return specLSQ.empty() && freeLdNum == maxLdNum && freeStNum == (maxStNum - comSQ.size()) && exLdNum == 0 && doneLdNum == 0;
}
