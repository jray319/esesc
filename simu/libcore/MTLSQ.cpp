#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "GMemorySystem.h"
#include "SescConf.h"
#include "DInst.h"

MTLSQ* MTLSQ::create(GProcessor *gproc_) {
	const char *memModel = SescConf->getCharPtr("cpusimu", "memModel", gproc_->getId());
	MTLSQ *ret = 0;

	if(!strcasecmp(memModel, "wmm")) {
		ret = new WMMLSQ(gproc_);
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
	, nLdKillByLd("P(%d)_MTLSQ_nLdKillByLd", gproc->getId())
	, nLdKillBySt("P(%d)_MTLSQ_nLdKillBySt", gproc->getId())
	, nLdKillByInv("P(%d)_MTLSQ_nLdKillByInv", gproc->getId())
	, nLdReExByLd("P(%d)_MTLSQ_nLdReExByLd", gproc->getId())
	, nLdReExBySt("P(%d)_MTLSQ_nLdReExBySt", gproc->getId())
	, nLdReExByInv("P(%d)_MTLSQ_nLdReExByInv", gproc->getId())
	, nStLdForward("P(%d)_MTLSQ_nStLdForward", gproc->getId())
	, nLdLdForward("P(%d)_MTLSQ_nLdLdForward", gproc->getId())
{
	SescConf->isInt("cpusimu", "maxLoads", gproc->getId());
	SescConf->isInt("cpusimu", "maxStores", gproc->getId());
	SescConf->isInt("cpusimu", "ldldForwardDelay", gproc->getId());
	SescConf->isInt("cpusimu", "stldForwardDelay", gproc->getId());
	SescConf->isBetween("cpusimu", "maxLoads", 1, 256, gproc->getId());
	SescConf->isBetween("cpusimu", "maxStores", 1, 256, gproc->getId());
	SescConf->isBetween("cpusimu", "ldldForwardDelay", 1, 5, gproc->getId());
	SescConf->isBetween("cpusimu", "stldForwardDelay", 1, 5, gproc->getId());

	I(gproc);
	I(mtStoreSet);
	I(DL1);

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

