#include "FrontEnd.h"
#include "SescConf.h"

FrontEnd::FrontEnd(FlowID id)
	: bpred(0)
	, FetchWidth(SescConf->getInt("cpusimu", "fetchWidth", id))
	, Delay(SescConf->getInt("cpusimu", "frontEndLat", id))
		// [sizhuo] size of instQ is extended with front-end pipeline stages
	, maxInstQSize(SescConf->getInt("cpusimu", "instQueueSize", id) + Delay)
	, freeInstQSize(maxInstQSize)
	, instQ(maxInstQSize)
	, missBranch(0)
	, flushing(false)
	, bucketPool(Delay + 1)
{
  SescConf->isInt("cpusimu", "fetchWidth",id);
  SescConf->isInt("cpusimu", "frontEndLat",id);
  SescConf->isBetween("cpusimu", "fetchWidth", 1, 1024, id);
  SescConf->isBetween("cpusimu", "frontEndLat", 1, 1024, id);

  const char *bpredSection = SescConf->getCharPtr("cpusimu","bpred",id);
	bpred = new BPredictor(id, FetchWidth, bpredSection);

	for(uint32_t i = 0; i <= Delay; i++) {
		InstBucket *buck = 0;
		buck = new InstBucket(FetchWidth);
		I(buck);
		bucketPool.push(buck);
	}
}

FrontEnd::~FrontEnd() {
	delete bpred;
}

void FrontEnd::fetch(EmulInterface *eint, FlowID fid) {
	if(flushing || missBranch != 0) {
		// [sizhuo] flusing or waiting mispred to resolve, no fetch
		return;
	}

	// [sizhuo] now start to fetch inst (limit by Fetch Width & instQ size)
	int32_t n2Fetch = FetchWidth; // [sizhuo] remaining fetch bandwidth
	InstBucket *buck = bucketPool.top();
	I(buck->empty());
	while(n2Fetch > 0 && freeInstQSize > 0) {
		// [sizhuo] get inst from emulator
		DInst *dinst = eint->executeHead(fid);
		if(dinst == 0) {
			break;
		}

		// [sizhuo] go into inst bucket
		buck->push(dinst);
		n2Fetch--;
		freeInstQSize--; // [sizhuo] dec free instQ size in advance
		I(freeInstQSize >= 0);
		I(n2Fetch >= 0);

		// [sizhuo] for branch inst, do prediction & stall on mispred
		if(dinst->getInst()->isControl()) {
			PredType pred = bpred->predict(dinst, true);
			if(pred == CorrectPrediction) {
				if(dinst->isTaken()) {
					// [sizhuo] we only fetch 1 basic block, stop here
					break;
				}
			} else {
				// [sizhuo] set mispred branch & reset in branch resource
				missBranch = dinst;
				dinst->lockFrontEnd(this);
				break;
			}
		}
	}

	// [sizhuo] if we fetch something, enq it to instQ after delay
	if(!buck->empty()) {
		I(n2Fetch != FetchWidth);
		bucketPool.pop(); // [sizhuo] remove bucket from pool
		enqInstQCB::schedule(Delay, this, buck);
	}
}

void FrontEnd::enqInstQ(InstBucket *buck) {
	// [sizhuo] move inst in bucket to instQ
	I(!buck->empty());
	while(!buck->empty()) {
		instQ.push(buck->top());
		buck->pop();
	}
	// [sizhuo] recycle inst bucket
	I(buck->empty());
	bucketPool.push(buck);
}

void FrontEnd::unblock(DInst *dinst) {
	if(dinst == missBranch) {
		missBranch = 0;
	}
}

