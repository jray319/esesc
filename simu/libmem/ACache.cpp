#include "nanassert.h"

#include "SescConf.h"
#include "MemorySystem.h"
#include "ACache.h"
#include "MSHR.h"
#include "GProcessor.h"
#include "TaskHandler.h"
#include "MemRequest.h"
#include "PortManager.h"
#include "../libsampler/BootLoader.h"
#include <string.h>

ACache::ACache(MemorySystem *gms, const char *section, const char *name)
	: MemObj(section, name)
	, reqFromUpPort(0)
	, respFromUpPort(0)
	, fromDownPort(0)
	, cache(0)
	, mshr(0)
	, tagDelay  (SescConf->getInt(section, "tagDelay"))
	, dataDelay (SescConf->getInt(section, "dataDelay"))
	, goUpDelay (SescConf->getInt(section, "goUpDelay"))
	, goDownDelay (SescConf->getInt(section, "goDownDelay"))
	, isL1 (SescConf->getBool(section, "isL1"))
	, isLLC (SescConf->getBool(section, "isLLC"))
	, upNodeNum(SescConf->getInt(section, "upNodeNum"))
{
	// [sizhuo] check delay
  SescConf->isGT(section, "tagDelay", 0);
  SescConf->isGT(section, "dataDelay", 0);
  SescConf->isGT(section, "goUpDelay", 0);
  SescConf->isGT(section, "goDownDelay", 0);

	// [sizhuo] check LLC & L1 params
	SescConf->isBool(section, "isLLC");
	SescConf->isBool(section, "isL1");

	// [sizhuo] check upper level node num
	SescConf->isGT(section, "upNodeNum", 0);
	I(upNodeNum >= 1);

	// [sizhuo] create inports
	reqFromUpPort = new CacheInport*[upNodeNum];
	respFromUpPort = new CacheInport*[upNodeNum];
	I(reqFromUpPort);
	I(respFromUpPort);
	char *portName = new char[strlen(name) + 100];
	for(int i = 0; i < upNodeNum; i++) {
		sprintf(portName, "%s_reqFromUpPort(%d)", name, i);
		reqFromUpPort[i] = 0;
		reqFromUpPort[i] = new FIFOCacheInport(portName);
		I(reqFromUpPort[i]);

		sprintf(portName, "%s_respFromUpPort(%d)", name, i);
		respFromUpPort[i] = 0;
		respFromUpPort[i] = new FIFOCacheInport(portName);
		I(respFromUpPort[i]);
	}
	sprintf(portName, "%s_fromDownPort", name);
	fromDownPort = new FIFOCacheInport(portName);
	I(fromDownPort);
	delete []portName;

	// [sizhuo] create cache tag array
	uint32_t cacheSize = SescConf->getInt(section, "Size");
	uint32_t lineSize = SescConf->getInt(section, "Bsize");
	uint32_t setAssoc = SescConf->getInt(section, "Assoc");
	cache = new CacheArray(cacheSize, lineSize, setAssoc);
	I(cache);
	ID(MSG("%s creates cache array: size %x, lineSize %x, assoc %x", name, cacheSize, lineSize, setAssoc));

	// [sizhuo] create MSHR
  const char* mshrSection = SescConf->getCharPtr(section,"MSHR");
	uint32_t mshrBankSize = SescConf->getInt(mshrSection, "nSubEntries");
	uint32_t mshrBankNum = SescConf->getInt(mshrSection, "size") / mshrBankSize;
	mshr = new HierMSHR(mshrBankNum, mshrBankSize, cache, name);
	I(mshr);
	ID(MSG("%s creates MSHR: bankNum %d, bankSize %d", name, mshrBankNum, mshrBankSize));

	// [sizhuo] create & add lower level component
  MemObj *lower_level = gms->declareMemoryObj(section, "lowerLevel");
  if(lower_level) {
    addLowerLevel(lower_level);
	} else {
		// [sizhuo] cache must have a valid lower level
		SescConf->notCorrect();
	}
}

ACache::~ACache() {
	if(reqFromUpPort) {
		for(int i = 0; i < upNodeNum; i++) {
			if(reqFromUpPort[i]) delete reqFromUpPort[i];
		}
		delete[]reqFromUpPort;
	}
	if(respFromUpPort) {
		for(int i = 0; i < upNodeNum; i++) {
			if(respFromUpPort[i]) delete respFromUpPort[i];
		}
		delete[]respFromUpPort;
	}
	if(fromDownPort) {
		delete fromDownPort;
	}
	if(cache) delete cache;
	if(mshr) delete mshr;
}

void ACache::req(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("req"));

  // predicated ARM instructions can be with zero address
  if(mreq->getAddr() == 0) {
    mreq->ack(tagDelay + dataDelay);
    return;
  }

	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	if(isL1) {
		I(mreq->isHomeNode());
		// [sizhuo] L1$ only has 1 upper node: LSU
		// enq msg to inport & record inport & set pos
		mreq->pos = MemRequest::Inport;
		mreq->inport = reqFromUpPort[0];
		I(mreq->inport);
		reqFromUpPort[0]->enqNewMsg(&(mreq->redoReqCB), mreq->getStatsFlag());
	} else {
		int portId = router->getCreatorPort(mreq);
		I(portId < upNodeNum);
		I(portId >= 0);
		// [sizhuo] enq msg to inport & record inport & set pos
		mreq->pos = MemRequest::Inport;
		mreq->inport = reqFromUpPort[portId];
		I(mreq->inport);
		reqFromUpPort[portId]->enqNewMsg(&(mreq->redoReqCB), mreq->getStatsFlag());
	}
}

void ACache::reqAck(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("reqAck"));
	I(!mreq->isRetrying());
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = fromDownPort;
	I(mreq->inport);
	fromDownPort->enqNewMsg(&(mreq->redoReqAckCB), mreq->getStatsFlag());
}

void ACache::setState(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("setState"));
	I(!mreq->isRetrying());
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = fromDownPort;
	I(mreq->inport);
	fromDownPort->enqNewMsg(&(mreq->redoSetStateCB), mreq->getStatsFlag());
}

void ACache::setStateAck(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("setStateAck"));
	I(!mreq->isRetrying());
	I(!isL1);
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = respFromUpPort[portId];
	I(mreq->inport);
	respFromUpPort[portId]->enqNewMsg(&(mreq->redoSetStateAckCB), mreq->getStatsFlag());
}

void ACache::disp(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("disp"));
	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = respFromUpPort[portId];
	I(mreq->inport);
	respFromUpPort[portId]->enqNewMsg(&(mreq->redoDispCB), mreq->getStatsFlag());
}

void ACache::doReq(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("doReq"));

	if(mreq->pos == MemRequest::Inport) {
		if(!mreq->isRetrying()) {
			// [sizhuo] new req from inport, add it to MSHR
			mshr->addUpReq(cache->getLineAddr(mreq->getAddr()), &(mreq->redoReqCB), mreq);
			// [sizhuo] wait until add success
			mreq->setRetrying();
			return;
		}
		// [sizhuo] add MSHR success
		// deq inport & set pos & clear retry bit
		I(mreq->isRetrying());
		mreq->clearRetrying();
		I(mreq->inport);
		mreq->inport->deqDoneMsg();
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		// [sizhuo] we proceed to handle this req
	}
	
	if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		// [sizhuo] the caline line in the same index to be replaced
		// XXX: if we replace random address not in same index we may have problem
		// because another active req in the same MSHR may be operating on it
		const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
		const AddrType repLineAddr = ((cache->getTag(lineAddr) + 1) << cache->log2Sets) | cache->getIndex(lineAddr);
		const AddrType repByteAddr = repLineAddr << cache->log2LineSize;
		I(cache->getLineAddr(repByteAddr) == repLineAddr);
		I(repLineAddr != lineAddr);
		I(cache->getIndex(repLineAddr) == cache->getIndex(lineAddr));

		if(!mreq->isRetrying()) {
			// [sizhuo] newly added to MSHR, try to replace a cache line
			// access tag & send invalidation to upper level
			int nmsg = router->invalidateAll(repByteAddr, mreq, tagDelay + goUpDelay);
			if(nmsg > 0) {
				I(mreq->hasPendingSetStateAck());
				// [sizhuo] wait for downgrade resp to wake me up
				mreq->setRetrying();
			} else {
				I(isL1); // [sizhuo] we must be in L1$
				I(!mreq->hasPendingSetStateAck());
				// [sizhuo] we re-handle this req after tag read
				mreq->setRetrying();
				(mreq->redoReqCB).schedule(tagDelay);
			}
		} else {
			// [sizhuo] invalidate upper level is done
			I(!mreq->hasPendingSetStateAck());
			mreq->clearRetrying(); // [sizhuo] clear retry bit
			// [sizhuo] send disp resp to lower level after reading data
			router->sendDisp(repByteAddr, mreq->getStatsFlag(), dataDelay + goDownDelay);
			// [sizhuo] forward req to lower level at the same time
			// set pos & notify MSHR that mreq goes down to lower level
			mshr->upReqToWait(lineAddr, dataDelay);
			mreq->pos = MemRequest::Router;
			router->scheduleReq(mreq, dataDelay + goDownDelay);
			// [sizhuo] we wait for reqAck to come back and completes this req in doReqAck()
		}
	} else {
		I(0);
	}
}

void ACache::doReqAck(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("doReqAck"));

	if(mreq->isHomeNode()) {
		// [sizhuo] this is home node, we can end the msg
		I(isL1); // [sizhuo] must be L1$
		// [sizhuo] deq msg from inport & set pos
		I(mreq->inport);
		mreq->inport->deqDoneMsg();	
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		// [sizhuo] immediately notify MSHR that ack comes back
		// (prevent downgrade req to this cache line from being accepted)
		mshr->upReqToAck(cache->getLineAddr(mreq->getAddr()));
		// [sizhuo] end this msg after data read & retire MSHR
		mshr->retireUpReq(cache->getLineAddr(mreq->getAddr()), dataDelay);
		mreq->ack(dataDelay + goUpDelay);
		return;
	}

	// [sizhuo] this cache cannot be L1$
	I(!isL1);
	if(isL1) MSG("ERROR: req ack reaches L1$ but not home node");

	if(mreq->pos == MemRequest::Inport) {
		// [sizhuo] new reqAck from inport, it has corresponding req in MSHR
		// deq inport & set pos & notify MSHR that reqAck comes back
		I(mreq->inport);
		mreq->inport->deqDoneMsg();
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		mshr->upReqToAck(cache->getLineAddr(mreq->getAddr()));
		// [sizhuo] we proceed to handle this msg next cycle
		(mreq->redoReqAckCB).schedule(1);
	} else if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		if(!mreq->isRetrying()) {
			// [sizhuo] send downgrade req to upper level except for upgrade req home node
			// for simplicity, we access tag again to send downgrade req
			int32_t nmsg = router->sendSetStateOthers(mreq, ma_setInvalid, tagDelay + goUpDelay);
			if(nmsg > 0) {
				I(mreq->hasPendingSetStateAck());
				// [sizhuo] need to wait for downgrade resp to try again
				mreq->setRetrying();
			} else {
				I(!mreq->hasPendingSetStateAck());
				// [sizhuo] no downgrade req sent, re-handle after tag read
				mreq->setRetrying();
				(mreq->redoReqAckCB).schedule(tagDelay);
			}
		} else {
			// [sizhuo] downgrade upper level is done
			I(!mreq->hasPendingSetStateAck());
			mreq->clearRetrying(); // [sizhuo] clear retry bit
			// [sizhuo] resp to upper level after data read & retire MSHR entry & set pos
			mshr->retireUpReq(cache->getLineAddr(mreq->getAddr()), dataDelay);
			mreq->pos = MemRequest::Router;
			router->scheduleReqAck(mreq, dataDelay + goUpDelay);
		}
	} else {
		I(0);
	}
}

void ACache::doSetState(MemRequest *mreq) {
	I(mreq);
  I(!mreq->isHomeNode()); // [sizhuo] home node should be at lower level
	ID(mreq->dump("doSetState"));

	if(mreq->pos == MemRequest::Inport) {
		if(!mreq->isRetrying()) {
			// [sizhuo] new downgrade req from inport, add to MSHR
			mshr->addDownReq(cache->getLineAddr(mreq->getAddr()), &(mreq->redoSetStateCB), mreq);
			// [sizhuo] wait until add success
			mreq->setRetrying();
			return;
		}
		// [sizhuo] add MSHR success
		// deq inport & set pos & clear retry bit
		I(mreq->isRetrying());
		mreq->clearRetrying();
		I(mreq->inport);
		mreq->inport->deqDoneMsg();
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		// [sizhuo] we proceed to handle this req
	}
	
	if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		if(!mreq->isRetrying()) {
			// [sizhuo] send downgrade req to upper level
			int32_t nmsg = router->sendSetStateAll(mreq, mreq->getAction(), tagDelay + goUpDelay);
			if(nmsg > 0) {
				I(mreq->hasPendingSetStateAck());
				// [sizhuo] need to wait for downgrade resp to try again
				mreq->setRetrying();
			} else {
				I(!mreq->hasPendingSetStateAck());
				// [sizhuo] we rehandle this req after tag delay
				mreq->setRetrying();
				(mreq->redoSetStateCB).schedule(tagDelay);
			}
		} else {
			// [sizhuo] downgrade is done
			I(!mreq->hasPendingSetStateAck());
			mreq->clearRetrying(); // [sizhuo] reset retry bit
			// [sizhuo] resp to lower level after data read & set pos & retire MSHR entry
			mshr->retireDownReq(cache->getLineAddr(mreq->getAddr()), dataDelay);
			mreq->pos = MemRequest::Router;
			mreq->convert2SetStateAck(ma_setInvalid);
			router->scheduleSetStateAck(mreq, dataDelay + goDownDelay);
		}
	} else {
		I(0);
	}
}

void ACache::doSetStateAck(MemRequest *mreq) {
	I(mreq);
	I(mreq->isHomeNode()); // [sizhuo] we must be at home node now
	GMSG(!mreq->isHomeNode(), "ERROR: SetStateAck arrives non-home node!");
	ID(mreq->dump("doSetStateAck"));

	I(!isL1);

	// [sizhuo] process msg success, deq it before mreq->ack destroy mreq
	// no need to set pos
	I(mreq->inport);
	mreq->inport->deqDoneMsg();	
	mreq->inport = 0;

	// [sizhuo] we can end this msg, setStateAckDone() may be called
	// and invoke other handler in this cache (delay is 0 here)
	// no need to set pos here
	const MemRequest *orig = mreq->getSetStateAckOrig();
	if(orig->isReqAck()) {
		I(orig->isRetrying()); // [sizhuo] must be a second handle of ReqAck
		// [sizhuo] we may handle doReqAck for orig again, no additional delay
		mreq->ack();
	} else if(orig->isSetState()) {
		I(orig->isRetrying());
		// [sizhuo] we may handle doSetState for orig again, no additional delay
		mreq->ack();
	} else if(orig->isReq()) {
		I(orig->isRetrying());
		// [sizhuo] we may handle doReq for orig again, no additional delay
		mreq->ack();
	} else {
		I(0);
		MSG("Unknown set state orig msg");
		mreq->ack();
	}
}

void ACache::doDisp(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("doDisp"));

	// [sizhuo] process msg success, deq it before mreq->ack destroy mreq
	// no need to set pos here
	I(mreq->inport);
	mreq->inport->deqDoneMsg();
	mreq->inport = 0;

	mreq->ack();
}

TimeDelta_t ACache::ffread(AddrType addr) {
	return tagDelay + dataDelay;
}

TimeDelta_t ACache::ffwrite(AddrType addr) {
	return tagDelay + 1;
}

bool ACache::isBusy(AddrType addr) const {
	return false;
}
