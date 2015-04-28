#include "HierMSHR.h"
#include "CacheInport.h"
#include "Snippets.h"
#include <algorithm>
#include <string.h>

#include "IndexSplitMSHRBank.h"
#include "FullSplitMSHRBank.h"

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
		insertFail[i] = 0;
		handleLat[i] = 0;
		for(int j = 0; j < MSHRBank::MaxIssueSC; j++) {
			issueFail[i][j] = 0;
			issueStall[i][j] = 0;
		}
	}
	avgMissLat[ma_setValid] = new GStatsAvg("%s_readMissLat", name);
	avgMissLat[ma_setDirty] = new GStatsAvg("%s_writeMissLat", name);
	avgMissLat[ma_setExclusive] = new GStatsAvg("%s_prefetchMissLat", name);
	I(avgMissLat[ma_setValid]);
	I(avgMissLat[ma_setDirty]);
	I(avgMissLat[ma_setExclusive]);

	char msgStr[ma_MAX][50];
	for(int i = 0; i < ma_MAX; i++) {
		msgStr[i][0] = 0;
	}
	sprintf(msgStr[ma_setValid], "read");
	sprintf(msgStr[ma_setDirty], "write");
	sprintf(msgStr[ma_setExclusive], "prefetch");
	sprintf(msgStr[ma_setInvalid], "setInvalid");
	sprintf(msgStr[ma_setShared], "setShared");

	char issueSCStr[MSHRBank::MaxIssueSC][50];
	sprintf(issueSCStr[MSHRBank::Insert], "Insert");
	sprintf(issueSCStr[MSHRBank::Downgrade], "Downgrade");
	sprintf(issueSCStr[MSHRBank::Upgrade], "Upgrade");
	sprintf(issueSCStr[MSHRBank::Replace], "Replace");
	sprintf(issueSCStr[MSHRBank::ReqNum], "ReqNum");

	for(int i = 0; i < ma_MAX; i++) {
		if(msgStr[i][0] != 0) {
			insertFail[i] = new GStatsCntr("%s_%sInsertFail", name, msgStr[i]);
			I(insertFail[i]);
			handleLat[i] = new GStatsAvg("%s_%sHandleLat", name, msgStr[i]);
			I(handleLat[i]);

			for(int j = 0; j < MSHRBank::MaxIssueSC; j++) {
				issueFail[i][j] = new GStatsCntr("%s_%sIssueFailBy_%s", name, msgStr[i], issueSCStr[j]);
				I(issueFail[i][j]);
				issueStall[i][j] = new GStatsCntr("%s_%sIssueStallBy_%s", name, msgStr[i], issueSCStr[j]);
				I(issueStall[i][j]);
			}
		}
	}

	// [sizhuo] create banks
	bank = new MSHRBank*[bankNum];
	I(bank);
	for(uint32_t i = 0; i < bkNum; i++) {
		bank[i] = 0;
		bank[i] = new FullSplitMSHRBank(this, i, bkUpSize, bkDownSize, c, name);
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

