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
#include <algorithm>

// TODO:
// 1*. currently data delay is added when req turns to ack,
// we may need more detailed modelling in future

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
	// [sizhuo] stats counters
	, displaced("%s:displaced", name)
	, writeBack("%s:writeBack", name)
	, avgMemLat("%s_avgMemLat", name)
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
		if(isL1) {
			// [sizhuo] L1 D$ can send req OOO, not in FIFO order
			reqFromUpPort[i] = new OOOCacheInport(portName);
		} else {
			reqFromUpPort[i] = new FIFOCacheInport(portName);
		}
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
	uint32_t bankNum = SescConf->getInt(section, "numBanks");
	cache = new LRUCacheArray(cacheSize, lineSize, setAssoc, bankNum, upNodeNum, name);
	I(cache);
	MSG("ACache %s creates cache array: size %x, lineSize %x, assoc %x, bankNum %d, upNodeNum %d", name, cacheSize, lineSize, setAssoc, bankNum, upNodeNum);

	// [sizhuo] create MSHR
  const char* mshrSection = SescConf->getCharPtr(section,"MSHR");
	uint32_t mshrBankSize = SescConf->getInt(mshrSection, "nSubEntries");
	uint32_t mshrBankNum = SescConf->getInt(mshrSection, "size") / mshrBankSize;
	mshr = new HierMSHR(mshrBankNum, mshrBankSize, cache, name);
	I(mshr);

	// [sizhuo] create & add lower level component
  MemObj *lower_level = gms->declareMemoryObj(section, "lowerLevel");
  if(lower_level) {
    addLowerLevel(lower_level);
	} else {
		// [sizhuo] cache must have a valid lower level
		SescConf->notCorrect();
	}

	// [sizhuo] stats counters
	for(int i = 0; i < ma_MAX; i++) {
		reqNum[i] = 0;
		reqHit[i] = 0;
		reqMiss[i] = 0;
		reqHalfMiss[i] = 0;
	}

	reqNum[ma_setValid] = new GStatsCntr("%s:setValidsetState", name);
	reqNum[ma_setDirty] = new GStatsCntr("%s:setDirtysetState", name);
	reqNum[ma_setExclusive] = new GStatsCntr("%s:setExclusivesetState", name);
	I(reqNum[ma_setValid]);
	I(reqNum[ma_setDirty]);
	I(reqNum[ma_setExclusive]);

	reqHit[ma_setValid] = new GStatsCntr("%s:readHit", name);
	reqHit[ma_setDirty] = new GStatsCntr("%s:writeHit", name);
	reqHit[ma_setExclusive] = new GStatsCntr("%s:setExclusiveHit", name);
	I(reqHit[ma_setValid]);
	I(reqHit[ma_setDirty]);
	I(reqHit[ma_setExclusive]);

	reqMiss[ma_setValid] = new GStatsCntr("%s:readMiss", name);
	reqMiss[ma_setDirty] = new GStatsCntr("%s:writeMiss", name);
	reqMiss[ma_setExclusive] = new GStatsCntr("%s:setExclusiveMiss", name);
	I(reqMiss[ma_setValid]);
	I(reqMiss[ma_setDirty]);
	I(reqMiss[ma_setExclusive]);

	reqHalfMiss[ma_setDirty] = new GStatsCntr("%s:writeHalfMiss", name);
	reqHalfMiss[ma_setExclusive] = new GStatsCntr("%s:setExclusiveHalfMiss", name);
	I(reqHalfMiss[ma_setDirty]);
	I(reqHalfMiss[ma_setExclusive]);
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

	const bool doStats = mreq->getStatsFlag();

	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	if(isL1) {
		I(mreq->isHomeNode());
		// [sizhuo] L1$ only has 1 upper node: LSU
		// enq msg to inport & record inport & set pos
		mreq->pos = MemRequest::Inport;
		mreq->inport = reqFromUpPort[0];
		I(mreq->inport);
		reqFromUpPort[0]->enqNewMsg(&(mreq->redoReqCB), doStats);
	} else {
		int portId = router->getCreatorPort(mreq);
		I(portId < upNodeNum);
		I(portId >= 0);
		// [sizhuo] enq msg to inport & record inport & set pos
		mreq->pos = MemRequest::Inport;
		mreq->inport = reqFromUpPort[portId];
		I(mreq->inport);
		reqFromUpPort[portId]->enqNewMsg(&(mreq->redoReqCB), doStats);
	}

	// [sizhuo] stats for req num
	I(reqNum[mreq->getAction()]);
	reqNum[mreq->getAction()]->inc(doStats);
}

void ACache::reqAck(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("reqAck"));
	I(!mreq->isRetrying());

	const bool doStats = mreq->getStatsFlag();
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = fromDownPort;
	I(mreq->inport);
	fromDownPort->enqNewMsg(&(mreq->redoReqAckCB), doStats);
}

void ACache::setState(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("setState"));
	I(!mreq->isRetrying());

	const bool doStats = mreq->getStatsFlag();
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = fromDownPort;
	I(mreq->inport);
	fromDownPort->enqNewMsg(&(mreq->redoSetStateCB), doStats);
}

void ACache::setStateAck(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("setStateAck"));
	I(!mreq->isRetrying());
	I(!isL1);

	const bool doStats = mreq->getStatsFlag();
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = respFromUpPort[portId];
	I(mreq->inport);
	respFromUpPort[portId]->enqNewMsg(&(mreq->redoSetStateAckCB), doStats);
}

void ACache::disp(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("disp"));
	I(!mreq->isRetrying());

	const bool doStats = mreq->getStatsFlag();
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	// [sizhuo] enq msg to inport & record inport & set pos
	mreq->pos = MemRequest::Inport;
	mreq->inport = respFromUpPort[portId];
	I(mreq->inport);
	respFromUpPort[portId]->enqNewMsg(&(mreq->redoDispCB), doStats);
}

void ACache::forwardReqDown(MemRequest *mreq, AddrType lineAddr, TimeDelta_t lat) {
	// [sizhuo] send send to lower level
	// we also need to notify MSHR & set pos
	// set address in cache line & clear current line
	I(mreq);
	I(mreq->line);
	mreq->line->lineAddr = lineAddr;
	mreq->line = 0;
	mshr->upReqToWait(lineAddr, lat);
	mreq->pos = MemRequest::Router;
	router->scheduleReq(mreq, lat + goDownDelay);
}

void ACache::doReq(MemRequest *mreq) {
	I(mreq);
	I(mreq->isUpgradeAction());
	ID(mreq->dump("doReq"));

	const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
	const MsgAction reqAct = mreq->getAction();
	const bool doStats = mreq->getStatsFlag();

	if(mreq->pos == MemRequest::Inport) {
		if(!mreq->isRetrying()) {
			// [sizhuo] new req from inport, add it to MSHR
			mshr->addUpReq(lineAddr, &(mreq->redoReqCB), mreq->inport, mreq);
			// [sizhuo] wait until add success
			mreq->setRetrying();
			return;
		}
		// [sizhuo] add MSHR success, set pos & clear retry bit
		I(mreq->isRetrying());
		mreq->clearRetrying();
		I(mreq->inport);
		//mreq->inport->deqDoneMsg(); // deq is done in MSHR
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		// [sizhuo] we proceed to handle this req
	}
	
	if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		if(!mreq->isRetrying()) {
			// [sizhuo] newly added to MSHR, access tag array
			I(mreq->line == 0);
			mreq->line = cache->upReqOccupyLine(lineAddr, mreq);
			I(mreq->line);
			// [sizhuo] get tag read out time delay
			const TimeDelta_t tagReadDelay = cache->getTagAccessTime(lineAddr, doStats) - globalClock + tagDelay;
			I(tagReadDelay >= tagDelay);

			const CacheLine::MESI lineState = mreq->line->state;

			// [sizhuo] perform different operations based on tag & req type
			if(mreq->line->lineAddr != lineAddr) {
				// [sizhuo] cache miss, incr miss stats counter
				I(reqMiss[reqAct]);
				reqMiss[reqAct]->inc(doStats);
				// [sizhuo] handle miss
				if(lineState != CacheLine::I) {
					// [sizhuo] we need to replace this line, first invalidate upper level
					// incr displace counter
					displaced.inc(doStats);
					// [sizhuo] handle replacement
					if(!isL1) {
						// [sizhuo] non-L1$ has upper level caches, need invalidation
						const AddrType repLineAddr = mreq->line->lineAddr;
						const AddrType repByteAddr = repLineAddr << cache->log2LineSize;
						I(cache->getLineAddr(repByteAddr) == repLineAddr);
						I(cache->getIndex(repLineAddr) == cache->getIndex(lineAddr));
						// [sizhuo] send invalidate msg to upper level
						I(upNodeNum == router->getUpNodeNum()); // [sizhuo] check up node num
						int nmsg = router->invalidateAllDir(repByteAddr, mreq, mreq->line->dir, tagReadDelay + goUpDelay);
						if(nmsg > 0) {
							I(mreq->hasPendingSetStateAck());
							// [sizhuo] wait for downgrade resp to wake me up
							mreq->setRetrying();
						} else {
							I(!mreq->hasPendingSetStateAck());
							// [sizhuo] no inv msg sent, we re-handle this req after tag read
							mreq->setRetrying();
							(mreq->redoReqCB).schedule(tagReadDelay);
						}
					} else {
						// [sizhuo] L1$ doesn't have upper level cache, re-handle after tag read
						mreq->setRetrying();
						(mreq->redoReqCB).schedule(tagReadDelay);
					}
				} else {
					// [sizhuo] replaced line is I. no need for replacement
					// but need to send req to lower level
					forwardReqDown(mreq, lineAddr, tagReadDelay);
				}
			} else {
				// [sizhuo] occupied line is in same address (but maybe I), check permission
				if(CacheLine::compatibleUpReq(mreq->line->state, reqAct, isLLC)) {
					// [sizhuo] state is compatible with req, no need to go to lower level
					// incr req hit stats counter
					I(reqHit[reqAct]);
					reqHit[reqAct]->inc(doStats);
					// [sizhuo] XXX: DON'T convert to ack now! 
					// otherwise redoReqAck() will be invoked when downgrade resp all comes back
					if(!isL1) {
						// [sizhuo] non-L1$, downgrade upper level (other than the initiator)
						const MsgAction downAct = reqAct == ma_setValid ? ma_setShared : ma_setInvalid;
						I(upNodeNum == router->getUpNodeNum()); // [sizhuo] check up node num
						int nmsg = router->sendSetStateOthersDir(mreq, downAct, mreq->line->dir, tagReadDelay + goUpDelay);
						if(nmsg > 0) {
							I(mreq->hasPendingSetStateAck());
							// [sizhuo] wait for downgrade resp to wake me up
							mreq->setRetrying();
						} else {
							I(!mreq->hasPendingSetStateAck());
							// [sizhuo] no msg sent, we re-handle this req after tag read
							mreq->setRetrying();
							(mreq->redoReqCB).schedule(tagReadDelay);
						}
					} else {
						// [sizhuo] L1$ doesn't have upper level cache, re-handle after tag read
						mreq->setRetrying();
						(mreq->redoReqCB).schedule(tagReadDelay);
					}
				} else {
					// [sizhuo] not enough permission (maybe I)
					// incr half miss OR miss stats counter
					if(mreq->line->state == CacheLine::I) {
						I(reqMiss[reqAct]);
						reqMiss[reqAct]->inc(doStats);
					} else {
						I(mreq->line->state == CacheLine::S);
						I(!isLLC);
						I(reqHalfMiss[reqAct]);
						reqHalfMiss[reqAct]->inc(doStats);
					}
					// [sizhuo] forward req down to lower level
					forwardReqDown(mreq, lineAddr, tagReadDelay);
				}
			}
		} else {
			// [sizhuo] downgrade upper level is done OR not needed
			I(!mreq->hasPendingSetStateAck());
			mreq->clearRetrying(); // [sizhuo] clear retry bit

			I(mreq->line);
			if(mreq->line->lineAddr != lineAddr) {
				const CacheLine::MESI oldState = mreq->line->state;
				// [sizhuo] cache line is replaced, change its state
				mreq->line->state = CacheLine::I;
				// [sizhuo] get delay in writing tag
				const TimeDelta_t tagWriteDelay = cache->getTagAccessTime(lineAddr, doStats) - globalClock;
				I(tagWriteDelay >= 0);

				if (oldState == CacheLine::M) {
					// [sizhuo] need write back, incr stats counter
					writeBack.inc(doStats);
					// [sizhuo] we need to send disp msg to lower level together with data
					// note: E state will be converted to M when recv down resp with data
					const AddrType repLineAddr = mreq->line->lineAddr;
					const AddrType repByteAddr = repLineAddr << cache->log2LineSize;
					I(cache->getLineAddr(repByteAddr) == repLineAddr);
					I(cache->getIndex(repLineAddr) == cache->getIndex(lineAddr));
					// [sizhuo] we also need to read data
					const TimeDelta_t dataReadDelay = cache->getDataAccessTime(lineAddr, doStats) - globalClock + dataDelay;
					I(dataReadDelay >= dataDelay);
					// [sizhuo] we can only send disp msg & forward req after data read & tag write
					const TimeDelta_t maxDelay = std::max(dataReadDelay, tagWriteDelay);
					// [sizhuo] send disp msg
					router->sendDisp(repByteAddr, doStats, maxDelay + goDownDelay);
					// [sizhuo] then forward req to lower level at the same time
					forwardReqDown(mreq, lineAddr, maxDelay + goDownDelay);
				} else {
					// [sizhuo] silently drop the line, directly go to lower level after tag write
					forwardReqDown(mreq, lineAddr, tagWriteDelay + goDownDelay);
				}
			} else {
				// [sizhuo] cache hit, we can start resp to upper level
				mreq->convert2ReqAck(reqAct);
				mshr->upReqToAck(lineAddr); // notify MSHR resp is here
				// [sizhuo] send resp
				if(mreq->isHomeNode()) {
					// [sizhuo] home node, we can end this msg, it must be L1
					I(isL1);
					I(!isLLC);
					// [sizhuo] we need to access data array 
					// for load -- we wait data delay to get data
					// for store -- we don't wait
					TimeDelta_t delay = cache->getDataAccessTime(lineAddr, doStats) - globalClock + (reqAct == ma_setValid ? dataDelay : 0);
					// [sizhuo] may need to update cache tag E->M
					if(reqAct == ma_setDirty && mreq->line->state != CacheLine::M) {
						I(mreq->line->state == CacheLine::E);
						mreq->line->state = CacheLine::M;
						// [sizhuo] update delay with tag write delay
						delay = std::max(delay, TimeDelta_t(cache->getTagAccessTime(lineAddr, doStats) - globalClock));
					}
					// [sizhuo] retire from MSHR & release occupation on cache line
					mreq->line->upReq = 0;
					mreq->line = 0;
					mshr->retireUpReq(lineAddr, delay);
					// [sizhuo] sample avg mem access latency
					avgMemLat.sample(mreq->getTimeDelay() + delay + goUpDelay, doStats);
					// [sizhuo] end this msg
					mreq->pos = MemRequest::Router;
					mreq->ack(goUpDelay + delay);
				} else {
					I(!isL1);
					// [sizhuo] not L1 home node, change directory
					int portId = router->getCreatorPort(mreq);
					I(portId < upNodeNum);
					I(portId >= 0);
					mreq->line->dir[portId] = CacheLine::upgradeState(reqAct);
					// [sizhuo] may change line state E->M
					if(reqAct == ma_setDirty) {
						mreq->line->state = CacheLine::M;
					}
					// [sizhuo] we have delay in writing tag & reading data, we choose the maximum delay
					const TimeDelta_t delay = std::max(cache->getTagAccessTime(lineAddr, doStats), cache->getDataAccessTime(lineAddr, doStats) + dataDelay) - globalClock;
					// [sizhuo] retire from MSHR & release occupation on cache line after tag write & data read
					mreq->line->upReq = 0;
					mreq->line = 0;
					mshr->retireUpReq(lineAddr, delay);
					// [sizhuo] send ack
					mreq->pos = MemRequest::Router;
					router->scheduleReqAck(mreq, delay + goUpDelay);
				}
			}
		}
	} else {
		I(0);
	}
}

void ACache::doReqAck(MemRequest *mreq) {
	I(mreq);
	I(mreq->isUpgradeAction());
	ID(mreq->dump("doReqAck"));

	const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
	const MsgAction reqAckAct = mreq->getAction();
	const bool doStats = mreq->getStatsFlag();

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
		// [sizhuo] change cache line state
		I(mreq->line == 0);
		CacheLine *line = cache->upReqFindLine(lineAddr, mreq);
		I(line);
		line->state = CacheLine::upgradeState(reqAckAct);
		// [sizhuo] get delay in writing tag & data
		const TimeDelta_t delay = std::max(cache->getTagAccessTime(lineAddr, doStats), cache->getDataAccessTime(lineAddr, doStats)) - globalClock;
		// [sizhuo] sample avg mem access latency
		avgMemLat.sample(mreq->getTimeDelay() + delay + goUpDelay, doStats);
		// [sizhuo] retire MSHR & release occupation on cache line & end msg
		line->upReq = 0;
		mshr->retireUpReq(lineAddr, delay);
		mreq->ack(goUpDelay + delay);
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
		mshr->upReqToAck(lineAddr);
		// [sizhuo] we access tag again upgrade state & read directory
		I(mreq->line == 0);
		mreq->line = cache->upReqFindLine(lineAddr, mreq);
		I(mreq->line);
		mreq->line->state = CacheLine::upgradeState(reqAckAct);
		// [sizhuo] we proceed to handle this msg after tag delay
		const TimeDelta_t tagReadDelay = cache->getTagAccessTime(lineAddr, doStats) - globalClock + tagDelay;
		(mreq->redoReqAckCB).schedule(tagReadDelay);
	} else if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		if(!mreq->isRetrying()) {
			// [sizhuo] send downgrade req to upper level except for upgrade req home node
			I(upNodeNum == router->getUpNodeNum()); // [sizhuo] check up node num
			int32_t nmsg = router->sendSetStateOthersDir(mreq, ma_setInvalid, mreq->line->dir, goUpDelay);
			if(nmsg > 0) {
				I(mreq->hasPendingSetStateAck());
				// [sizhuo] need to wait for downgrade resp to try again
				mreq->setRetrying();
				return;
			}
			// [sizhuo] no downgrade req sent, proceed to send resp
			I(!mreq->hasPendingSetStateAck());
		}
		// [sizhuo] downgrade upper level is done OR not needed
		I(!mreq->hasPendingSetStateAck());
		mreq->clearRetrying(); // [sizhuo] clear retry bit
		// [sizhuo] change cache line state (may be redundant, but more robust)
		I(mreq->line);
		mreq->line->state = CacheLine::upgradeState(reqAckAct);
		// [sizhuo] change directory
		int portId = router->getCreatorPort(mreq);
		I(portId < upNodeNum);
		I(portId >= 0);
		mreq->line->dir[portId] = CacheLine::upgradeState(reqAckAct);
		// [sizhuo] get delay in writing tag & data
		const TimeDelta_t delay = std::max(cache->getTagAccessTime(lineAddr, doStats), cache->getDataAccessTime(lineAddr, doStats)) - globalClock;
		// [sizhuo] release occupation on cache line & retire from MSHR after data & tag write
		mreq->line->upReq = 0;
		mreq->line = 0;
		mshr->retireUpReq(lineAddr, delay);
		// [sizhuo] resp to upper level after & set pos
		mreq->pos = MemRequest::Router;
		router->scheduleReqAck(mreq, delay + goUpDelay);
	} else {
		I(0);
	}
}

void ACache::doSetState(MemRequest *mreq) {
	I(mreq);
  I(!mreq->isHomeNode()); // [sizhuo] home node should be at lower level
	ID(mreq->dump("doSetState"));

	const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
	const MsgAction reqAct = mreq->getAction();
	const bool doStats = mreq->getStatsFlag();

	if(mreq->pos == MemRequest::Inport) {
		if(!mreq->isRetrying()) {
			// [sizhuo] new downgrade req from inport, add to MSHR
			mshr->addDownReq(lineAddr, &(mreq->redoSetStateCB), mreq->inport, mreq);
			// [sizhuo] wait until add success
			mreq->setRetrying();
			return;
		}
		// [sizhuo] add MSHR success, set pos & clear retry bit
		I(mreq->isRetrying());
		mreq->clearRetrying();
		I(mreq->inport);
		//mreq->inport->deqDoneMsg(); // deq is done in MSHR
		mreq->inport = 0;
		mreq->pos = MemRequest::MSHR;
		// [sizhuo] we proceed to handle this req
	}
	
	if(mreq->pos == MemRequest::MSHR) {
		I(mreq->inport == 0);
		if(!mreq->isRetrying()) {
			// [sizhuo] access tag to occuppy the cache line
			I(mreq->line == 0);
			mreq->line = cache->downReqOccupyLine(lineAddr, mreq);
			// [sizhuo] get tag read delay
			const TimeDelta_t tagReadDelay = cache->getTagAccessTime(lineAddr, doStats) - globalClock + tagDelay;

			if(mreq->line == 0) {
				// [sizhuo] requested line is invalid, just resp after tag read
				mshr->retireDownReq(lineAddr, tagReadDelay);
				mreq->pos = MemRequest::Router;
				mreq->convert2SetStateAck(ma_setInvalid);
				mreq->downRespData = false; // [sizhuo] no data resp
				router->scheduleSetStateAck(mreq, tagReadDelay + goDownDelay);
			} else {
				if(CacheLine::compatibleDownReq(mreq->line->state, reqAct)) {
					// [sizhuo] state & req are compatible, must be S & setShared
					I(mreq->line->state == CacheLine::S);
					I(reqAct == ma_setShared);
					// [sizhuo] relese occupation on cache line
					mreq->line->downReq = 0;
					mreq->line = 0;
					// [sizhuo] retire MSHR & send resp after tag delay
					mshr->retireDownReq(lineAddr, tagReadDelay);
					mreq->pos = MemRequest::Router;
					mreq->convert2SetStateAck(ma_setShared);
					mreq->downRespData = false; // [sizhuo] no data resp
					router->scheduleSetStateAck(mreq, tagReadDelay + goDownDelay);
				} else {
					// [sizhuo] we need to do downgrade, first downgrade upper level
					if(isL1) {
						// [sizhuo] no upper level, re-handle after tag read
						mreq->setRetrying();
						(mreq->redoSetStateCB).schedule(tagReadDelay);
					} else {
						// [sizhuo] forward downgrade req to upper level
						I(upNodeNum == router->getUpNodeNum()); // [sizhuo] check up node num
						int32_t nmsg = router->sendSetStateAllDir(mreq, mreq->getAction(), mreq->line->dir, tagReadDelay + goUpDelay);
						if(nmsg > 0) {
							I(mreq->hasPendingSetStateAck());
							// [sizhuo] need to wait for downgrade resp to try again
							mreq->setRetrying();
						} else {
							I(!mreq->hasPendingSetStateAck());
							// [sizhuo] we rehandle this req after tag delay
							mreq->setRetrying();
							(mreq->redoSetStateCB).schedule(tagReadDelay);
						}
					}
				}
			}
		} else {
			// [sizhuo] downgrade upper level is done
			I(!mreq->hasPendingSetStateAck());
			mreq->clearRetrying(); // [sizhuo] reset retry bit
			I(mreq->line);
			I(mreq->line->state != CacheLine::I);
			// [sizhuo] calc delay & set data resp bit
			// we need to read data & send down when in M state
			TimeDelta_t delay = 0;
			mreq->downRespData = false;
			if(mreq->line->state == CacheLine::M) {
				// [sizhuo] read data & append data to resp msg
				delay = cache->getDataAccessTime(lineAddr, doStats) - globalClock + dataDelay;
				mreq->downRespData = true;
			}
			// [sizhuo] change cache line state
			mreq->line->state = CacheLine::downgradeState(reqAct);
			// [sizhuo] update delay with tag write delay
			delay = std::max(delay, TimeDelta_t(cache->getTagAccessTime(lineAddr, doStats) - globalClock));
			// [sizhuo] release cache line occupation
			mreq->line->downReq = 0;
			mreq->line = 0;
			// [sizhuo] resp to lower level & set pos & retire MSHR entry
			mshr->retireDownReq(lineAddr, delay);
			mreq->pos = MemRequest::Router;
			mreq->convert2SetStateAck(reqAct); // [sizhuo] use the original action
			router->scheduleSetStateAck(mreq, delay + goDownDelay);
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

	const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
	const MsgAction ackAct = mreq->getAction();
	const bool doStats = mreq->getStatsFlag();

	// [sizhuo] search cache to change directory
	CacheLine *line = cache->downRespFindLine(lineAddr);
	I(line);
	// [sizhuo] debug
	if(line == 0) {
		MSG("setStateAck fails to match cache line");
		mreq->dumpAlways();
		mreq->getSetStateAckOrig()->dumpAlways();
	}
	////
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	line->dir[portId] = CacheLine::downgradeState(ackAct);
	// [sizhuo] get tag write delay
	TimeDelta_t delay = cache->getTagAccessTime(lineAddr, doStats) - globalClock;
	// [sizhuo] check whether resp contains data
	if(mreq->downRespData) {
		// [sizhuo] change cache line state to M
		I(line->state == CacheLine::M || line->state == CacheLine::E);
		line->state = CacheLine::M;
		// [sizhuo] write data array, update delay
		delay = std::max(delay, TimeDelta_t(cache->getDataAccessTime(lineAddr, doStats) - globalClock));
	}

	// [sizhuo] process msg success, deq it before mreq->ack destroy mreq
	// no need to set pos
	I(mreq->inport);
	CacheInport::deqDoneMsgCB::schedule(delay, mreq->inport);
	mreq->inport = 0;

	// [sizhuo] we can end this msg, setStateAckDone() may be called
	// and invoke other handler in this cache (delay is 0 here)
	// no need to set pos here
	const MemRequest *orig = mreq->getSetStateAckOrig();
	if(orig->isReqAck()) {
		I(orig->isRetrying()); // [sizhuo] must be a second handle of ReqAck
		// [sizhuo] we may handle doReqAck for orig again, no additional delay
		mreq->ack(delay);
	} else if(orig->isSetState()) {
		I(orig->isRetrying());
		// [sizhuo] we may handle doSetState for orig again, no additional delay
		mreq->ack(delay);
	} else if(orig->isReq()) {
		I(orig->isRetrying());
		// [sizhuo] we may handle doReq for orig again, no additional delay
		mreq->ack(delay);
	} else {
		I(0);
		MSG("Unknown set state orig msg");
		mreq->ack(delay);
	}
}

void ACache::doDisp(MemRequest *mreq) {
	I(mreq);
	ID(mreq->dump("doDisp"));

	const AddrType lineAddr = cache->getLineAddr(mreq->getAddr());
	const bool doStats = mreq->getStatsFlag();

	// [sizhuo] search cache to change directory
	CacheLine *line = cache->downRespFindLine(lineAddr);
	I(line);
	// [sizhuo] debug
	if(line == 0) {
		MSG("setStateAck fails to match cache line");
		mreq->dumpAlways();
	}
	////
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	line->dir[portId] = CacheLine::I;
	// [sizhuo] we only get disp for dirty block, change cache line state to M
	I(line->state == CacheLine::M || line->state == CacheLine::E);
	line->state = CacheLine::M;
	// [sizhuo] delay of writing data & tag
	const TimeDelta_t delay = std::max(cache->getTagAccessTime(lineAddr, doStats), cache->getDataAccessTime(lineAddr, doStats)) - globalClock;

	// [sizhuo] process msg success, deq it before mreq->ack destroy mreq
	// no need to set pos here
	I(mreq->inport);
	CacheInport::deqDoneMsgCB::schedule(delay, mreq->inport);
	mreq->inport = 0;

	mreq->ack(delay);
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
