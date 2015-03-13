
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
	, tagDelay  (SescConf->getInt(section, "tagDelay"))
	, dataDelay (SescConf->getInt(section, "dataDelay"))
	, isL1 (SescConf->getBool(section, "isL1"))
	, isLLC (SescConf->getBool(section, "isLLC"))
	, upNodeNum(SescConf->getInt(section, "upNodeNum"))
{
	// [sizhuo] check delay
  SescConf->isGT(section, "tagDelay", 0);
  SescConf->isGT(section, "dataDelay", 0);
  SescConf->isGT(section, "fromUpDelay", 0);
  SescConf->isGT(section, "fromDownDelay", 0);

	// [sizhuo] check LLC & L1 params
	SescConf->isBool(section, "isLLC");
	SescConf->isBool(section, "isL1");

	// [sizhuo] check upper level node num
	SescConf->isGT(section, "upNodeNum", 0);
	I(upNodeNum >= 1);

	// [sizhuo] create inports
	const Time_t fromUpDelay = SescConf->getInt(section, "fromUpDelay");
	const Time_t fromDownDelay = SescConf->getInt(section, "fromDownDelay");
	reqFromUpPort = new CacheInport[upNodeNum];
	respFromUpPort = new CacheInport[upNodeNum];
	fromDownPort = new CacheInport; // [sizhuo] only 1 down node
	I(reqFromUpPort);
	I(respFromUpPort);
	I(fromDownPort);
	char *portName = new char[strlen(name) + 100];
	for(int i = 0; i < upNodeNum; i++) {
		sprintf(portName, "%s_reqFromUpPort(%d)", name, i);
		reqFromUpPort[i].setup(fromUpDelay, portName);
		sprintf(portName, "%s_respFromUpPort(%d)", name, i);
		respFromUpPort[i].setup(fromUpDelay, portName);
	}
	sprintf(portName, "%s_fromDownPort", name);
	fromDownPort->setup(fromDownDelay, portName);
	delete []portName;

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
}

void ACache::req(MemRequest *mreq) {
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
		reqFromUpPort[0].enqNewMsg(&(mreq->redoReqCB), mreq->getStatsFlag());
	} else {
		int portId = router->getCreatorPort(mreq);
		I(portId < upNodeNum);
		I(portId >= 0);
		reqFromUpPort[portId].enqNewMsg(&(mreq->redoReqCB), mreq->getStatsFlag());
	}
}

void ACache::reqAck(MemRequest *mreq) {
	ID(mreq->dump("reqAck"));

	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	fromDownPort->enqNewMsg(&(mreq->redoReqAckCB), mreq->getStatsFlag());
}

void ACache::setState(MemRequest *mreq) {
	ID(mreq->dump("setState"));

	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	fromDownPort->enqNewMsg(&(mreq->redoSetStateCB), mreq->getStatsFlag());
}

void ACache::setStateAck(MemRequest *mreq) {
	ID(mreq->dump("setStateAck"));

	I(!mreq->isRetrying());
	I(!isL1);
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	respFromUpPort[portId].enqNewMsg(&(mreq->redoSetStateAckCB), mreq->getStatsFlag());
}

void ACache::disp(MemRequest *mreq) {
	ID(mreq->dump("disp"));

	I(!mreq->isRetrying());
	// [sizhuo] enq new msg to inport
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);
	respFromUpPort[portId].enqNewMsg(&(mreq->redoDispCB), mreq->getStatsFlag());
}

void ACache::doReq(MemRequest *mreq) {
	ID(mreq->dump("doReq"));

	// [sizhuo] find portId
	int portId = 0;
	if(!isL1) { // [sizhuo] L1 only has 1 upper node: LSU
		portId = router->getCreatorPort(mreq);
		I(portId < upNodeNum);
		I(portId >= 0);
	}

	// [sizhuo] forward req to lower level, wait ReqAck and complete req at doReqAck
	router->scheduleReq(mreq, tagDelay);

	// [sizhuo] process msg success, deq it
	reqFromUpPort[portId].deqDoneMsg();	
}

void ACache::doReqAck(MemRequest *mreq) {
	ID(mreq->dump("doReqAck"));

	if(mreq->isHomeNode()) {
		// [sizhuo] this is home node, end the msg
		I(isL1); // [sizhuo] must be L1$
		mreq->ack(dataDelay);
		// [sizhuo] process msg success, deq it
		fromDownPort->deqDoneMsg();	
		return;
	} 

	// [sizhuo] this cache cannot be L1$
	I(!isL1);
	if(isL1) MSG("ERROR: req ack reaches L1$ but not home node");

	if(mreq->isRetrying()) {
		// [sizhuo] second time handling req ack
		// downgrade req has been done, resp to upper level
		mreq->clearRetrying();
		router->scheduleReqAck(mreq, 0);
		// [sizhuo] don't deq msg from inport, already deq before
		return;
	}

	// [sizhuo] broadcast to upper level except for the one with home node
	int32_t nmsg = router->sendSetStateOthers(mreq, ma_setInvalid, tagDelay);

	if(nmsg > 0) {
		I(mreq->hasPendingSetStateAck());
		// [sizhuo] need to wait for downgrade resp to handle req ack again
		mreq->setRetrying();
	} else {
		I(!mreq->hasPendingSetStateAck());
		I(!mreq->isRetrying());
		// [sizhuo] no broadcast is made, just resp
		router->scheduleReqAck(mreq, 0);
	}
	// [sizhuo] process msg success, deq it
	fromDownPort->deqDoneMsg();	
}

void ACache::doSetState(MemRequest *mreq) {
  I(!mreq->isHomeNode()); // [sizhuo] home node should be at lower level
	ID(mreq->dump("doSetState"));

	if(mreq->isRetrying()) {
		// [sizhuo] second time handling set state req
		// upper level already downgraded, just resp
		mreq->clearRetrying();
		mreq->convert2SetStateAck(ma_setInvalid);
		router->scheduleSetStateAck(mreq, 0);
		// [sizhuo] don't deq msg from inport, already deq before
		return;
	}

	// [sizhuo] broadcast to all upper level
	int32_t nmsg = router->sendSetStateAll(mreq, mreq->getAction(), tagDelay);

	if(nmsg > 0) {
		I(mreq->hasPendingSetStateAck());
		// [sizhuo] need to wait for downgrade resp to handle req ack again
		mreq->setRetrying();
	} else {
		I(!mreq->hasPendingSetStateAck());
		I(!mreq->isRetrying());
		// [sizhuo] no broadcast is made, just resp
		mreq->convert2SetStateAck(ma_setInvalid);
		router->scheduleSetStateAck(mreq, 0);
	}
	// [sizhuo] process msg success, deq it
	fromDownPort->deqDoneMsg();
}

void ACache::doSetStateAck(MemRequest *mreq) {
	I(mreq->isHomeNode()); // [sizhuo] we must be at home node now
	GMSG(!mreq->isHomeNode(), "ERROR: SetStateAck arrives non-home node!");
	ID(mreq->dump("doSetStateAck"));

	// [sizhuo] find port first (mreq is destroyed later)
	I(!isL1);
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);

	// [sizhuo] we can end this msg, setStateAckDone() may be called
	// depending on the type original msg that generates this ack
	// we have different delay added
	const MemRequest *orig = mreq->getSetStateAckOrig();
	if(orig->isReqAck()) {
		I(orig->isRetrying()); // [sizhuo] must be a second handle of ReqAck
		// [sizhuo] we may handle doReqAck for orig again, no additional delay
		mreq->ack();
	} else if(orig->isSetState()) {
		// [sizhuo] we may handle doSetState for orig again, no additional delay
		mreq->ack();
	} else {
		I(0);
		MSG("Unknown set state orig msg");
		mreq->ack();
	}

	// [sizhuo] process msg success, deq it
	respFromUpPort[portId].deqDoneMsg();	
}

void ACache::doDisp(MemRequest *mreq) {
	ID(mreq->dump("doDisp"));

	// [sizhuo] find port first (mreq is destroyed later)
	I(!isL1);
	int portId = router->getCreatorPort(mreq);
	I(portId < upNodeNum);
	I(portId >= 0);

	mreq->ack();

	// [sizhuo] process msg success, deq it
	respFromUpPort[portId].deqDoneMsg();	
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
