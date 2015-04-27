#include "HierMSHR.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

//#include "BlockMSHRBank.h"
#include "IndexSplitMSHRBank.h"

// HierMSHR class
HierMSHR::HierMSHR(uint32_t bkNum, int bkUpSize, int bkDownSize, CacheArray *c, const char *str)
	: bankNum(bkNum)
	, bankMask(bkNum - 1)
	, name(0)
	, cache(c)
	, bank(0)
{
	// [sizhuo] bank num must be power of 2
	I((0x01L << log2i(bankNum)) == bankNum);
	// [sizhuo] bank num & per bank size must > 0
	I(bankNum > 0);
	I(bkUpSize > 0);
	I(bkDownSize > 0);

	name = new char[strlen(str) + 20];
	I(name);
	sprintf(name, "%s_MSHR", str);

	// [sizhuo] create stats
	for(int i = 0; i < ma_MAX; i++) {
		avgMissLat[i] = 0;
		insertLat[i] = 0;
		insertFail[i] = 0;
		issueLat[i] = 0;
	}
	avgMissLat[ma_setValid] = new GStatsAvg("%s_readMissLat", name);
	avgMissLat[ma_setDirty] = new GStatsAvg("%s_writeMissLat", name);
	avgMissLat[ma_setExclusive] = new GStatsAvg("%s_prefetchMissLat", name);
	I(avgMissLat[ma_setValid]);
	I(avgMissLat[ma_setDirty]);
	I(avgMissLat[ma_setExclusive]);

	insertLat[ma_setValid] = new GStatsAvg("%s_readInsertLat", name);
	insertLat[ma_setDirty] = new GStatsAvg("%s_writeInsertLat", name);
	insertLat[ma_setExclusive] = new GStatsAvg("%s_prefetchInsertLat", name);
	insertLat[ma_setInvalid] = new GStatsAvg("%s_setInvalidInsertLat", name);
	insertLat[ma_setShared] = new GStatsAvg("%s_setSharedInsertLat", name);
	I(insertLat[ma_setValid]);
	I(insertLat[ma_setDirty]);
	I(insertLat[ma_setExclusive]);
	I(insertLat[ma_setInvalid]);
	I(insertLat[ma_setShared]);

	insertFail[ma_setValid] = new GStatsCntr("%s_readInsertFail", name);
	insertFail[ma_setDirty] = new GStatsCntr("%s_writeInsertFail", name);
	insertFail[ma_setExclusive] = new GStatsCntr("%s_prefetchInsertFail", name);
	insertFail[ma_setInvalid] = new GStatsCntr("%s_setInvalidInsertFail", name);
	insertFail[ma_setShared] = new GStatsCntr("%s_setSharedInsertFail", name);
	I(insertFail[ma_setValid]);
	I(insertFail[ma_setDirty]);
	I(insertFail[ma_setExclusive]);
	I(insertFail[ma_setInvalid]);
	I(insertFail[ma_setShared]);

	issueLat[ma_setValid] = new GStatsAvg("%s_readIssueLat", name);
	issueLat[ma_setDirty] = new GStatsAvg("%s_writeIssueLat", name);
	issueLat[ma_setExclusive] = new GStatsAvg("%s_prefetchIssueLat", name);
	issueLat[ma_setInvalid] = new GStatsAvg("%s_setInvalidIssueLat", name);
	issueLat[ma_setShared] = new GStatsAvg("%s_setSharedIssueLat", name);
	I(issueLat[ma_setValid]);
	I(issueLat[ma_setDirty]);
	I(issueLat[ma_setExclusive]);
	I(issueLat[ma_setInvalid]);
	I(issueLat[ma_setShared]);

	// [sizhuo] create banks
	bank = new MSHRBank*[bankNum];
	I(bank);
	for(uint32_t i = 0; i < bkNum; i++) {
		bank[i] = 0;
		//bank[i] = new BlockMSHRBank(i, c, name);
		bank[i] = new IndexSplitMSHRBank(this, i, bkUpSize, bkDownSize, c, name);
		I(bank[i]);
	}
}

HierMSHR::~HierMSHR() {
	if(bank) {
		for(uint32_t i = 0; i < bankNum; i++) {
			if(bank[i]) delete[]bank[i];
		}
		delete[]bank;
	}
	if(name) {
		delete[]name;
	}
}

