#include "MTStoreSet.h"
#include <algorithm>
#include "SescConf.h"

MTStoreSet* MTStoreSet::create(int32_t cpu_id) {
	const char *type = SescConf->getCharPtr("cpusimu", "storeSetType", cpu_id);
	MTStoreSet *ret = 0;
	if(!strcasecmp(type, "empty")) {
		ret = new EmptyMTStoreSet;
	} else if(!strcasecmp(type, "serial")) {
		ret = new SerialMTStoreSet;
	} else if(!strcasecmp(type, "full")) {
		ret = new FullMTStoreSet(cpu_id);
	} else {
		SescConf->notCorrect();
	}
	return ret;
}

const SSID_t FullMTStoreSet::invalidSSID = -1;

FullMTStoreSet::FullMTStoreSet(int32_t cpu_id)
	: ssit(0)
	, lfmt(0)
	// [sizhuo] only WMM orders loads to same addr
	, orderLdLd(!strcasecmp(SescConf->getCharPtr("cpusimu", "memModel", cpu_id), "wmm"))
	, ssitSize(SescConf->getInt("cpusimu", "ssitSize", cpu_id))
	, ssitMask(ssitSize - 1)
	, lfmtSize(SescConf->getInt("cpusimu", "lfmtSize", cpu_id))
	, lfmtMask(lfmtSize - 1)
	, clearCycle(SescConf->getInt("cpusimu", "ssClearCycle", cpu_id))
	, clearCB(this)
	, nextSSID(0)
{
	// [sizhuo] check param
	SescConf->isCharPtr("cpusimu", "memModel", cpu_id);
	SescConf->isInt("cpusimu", "ssitSize", cpu_id);
	SescConf->isInt("cpusimu", "lfmtSize", cpu_id);
	SescConf->isGT("cpusimu", "ssitSize", 0, cpu_id);
	SescConf->isGT("cpusimu", "lfmtSize", 0, cpu_id);
	SescConf->isPower2("cpusimu", "ssitSize", cpu_id);
	SescConf->isPower2("cpusimu", "lfmtSize", cpu_id);
	SescConf->isInt("cpusimu", "ssClearCycle", cpu_id);
	SescConf->isBetween("cpusimu", "ssClearCycle", 100000, 10000000, cpu_id);

	// [sizhuo] create tables
	ssit = new SSID_t[ssitSize];
	I(ssit);
	lfmt = new DInst*[lfmtSize];
	I(lfmt);

	// [sizhuo] clear tables & schedule periodical clear routine
	clear();

	MSG("INFO: create P(%d)_FullMTStoreSet, ssitSize %u, lfmtSize %u, orderLdLd %d", cpu_id, ssitSize, lfmtSize, orderLdLd);
}

FullMTStoreSet::~FullMTStoreSet() {
	if(ssit) {
		delete[]ssit;
		ssit = 0;
	}
	if(lfmt) {
		delete[]lfmt;
		lfmt = 0;
	}
}

void FullMTStoreSet::clear() {
	I(ssit);
	I(lfmt);
	for(uint32_t i = 0; i < ssitSize; i++) {
		ssit[i] = invalidSSID;
	}
	for(uint32_t i = 0; i < lfmtSize; i++) {
		lfmt[i] = 0;
	}
	clearCB.scheduleAbs(globalClock + clearCycle);
}

void FullMTStoreSet::insert(DInst *dinst) {
	I(dinst);
	I(dinst->getSSID() == invalidSSID);
	// [sizhuo] look up in SSIT for SSID
	const SSID_t id = ssit[getSSITIndex(dinst)];
	// [sizhuo] XXX: record SSID in dinst for remove when dinst is executed
	dinst->setSSID(id); 
	// [sizhuo] dinst has mem dependency only when SSID is valid
	if(id != invalidSSID) {
		I(id >= 0 && uint32_t(id) < lfmtSize);
		// [sizhuo] look up in LFMT
		if(lfmt[id]) {
			// [sizhuo] create mem dependency between dinst and lfmt[id]
			lfmt[id]->addMemDep(dinst);
		}
		// [sizhuo] store will overwrite LFMT
		// only in WMM model, load will overwrite LFMT to order loads on same addr
		if(dinst->getInst()->isStore() || orderLdLd) {
			lfmt[id] = dinst;
		}
	}
}

void FullMTStoreSet::remove(DInst *dinst) {
	I(dinst);
	// [sizhuo] XXX: we must use the SSID stored in dinst to clear LFMT
	// because SSIT entry may be overwritten
	const SSID_t id = dinst->getSSID();
	if(id != invalidSSID) {
		I(id >= 0 && uint32_t(id) < lfmtSize);
		if(lfmt[id] == dinst) {
			lfmt[id] = 0;
		}
	}
}

void FullMTStoreSet::memDepViolate(DInst *oldInst, DInst *youngInst) {
	const SSID_t oldID = oldInst->getSSID();
	const SSID_t youngID = youngInst->getSSID();

	SSID_t newID = invalidSSID; // [sizhuo] SSID for both inst

	if(oldID == invalidSSID && youngID == invalidSSID) {
		// [sizhuo] create new SSID
		newID = createSSID();
	} else if(oldID == invalidSSID && youngID != invalidSSID) {
		// [sizhuo] add old inst to young inst's store set
		newID = youngID;
	} else if(oldID != invalidSSID && youngID == invalidSSID) {
		// [sizhuo] add young inst to old inst's store set
		newID = oldID;
	} else {
		// [sizhuo] store set merge: select the smaller ID
		newID = std::min(oldID, youngID);
	}
	// [sizhuo] assign SSID to both inst 
	// (note: ssit may be overwritten by other inst before), so we write both
	I(newID >= 0 && uint32_t(newID) < lfmtSize);
	ssit[getSSITIndex(oldInst)] = newID;
	ssit[getSSITIndex(youngInst)] = newID;
	// [sizhuo] FIXME: do we need to clear LFMT[newID]?
	
	// [sizhuo] XXX: this violate may not result in a ROB flush
	// so we MUST NOT change dinst->SSID
	// otherwise oldInst/youngInst will fail to reset LFMT entry, causing deadlock
}

void FullMTStoreSet::reset() {
	// [sizhuo] clear LFMT
	for(uint32_t i = 0; i < lfmtSize; i++) {
		lfmt[i] = 0;
	}
}

bool FullMTStoreSet::isReset() {
	// [sizhuo] check whether LFMT is all 0
	for(uint32_t i = 0; i < lfmtSize; i++) {
		if(lfmt[i]) {
			return false;
		}
	}
	return true;
}
