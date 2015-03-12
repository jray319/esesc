
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

ACache::ACache(MemorySystem *gms, const char *section, const char *name)
	: MemObj(section, name)
	, tagDelay  (SescConf->getInt(section,"tagDelay"))
	, dataDelay (SescConf->getInt(section,"dataDelay"))
	, goUpDelay (SescConf->getInt(section,"goUpDelay"))
	, goDownDelay (SescConf->getInt(section,"goDownDelay"))
	, isL1 (SescConf->getBool(section, "isL1"))
	, isLLC (SescConf->getBool(section, "isLLC"))
{
	// [sizhuo] check delay
  SescConf->isGT(section, "tagDelay" ,0);
  SescConf->isGT(section, "dataDelay",0);
  SescConf->isGT(section, "goUpDelay",0);
  SescConf->isGT(section, "goDownDelay",0);

	// [sizhuo] check LLC & L1 params
	SescConf->isBool(section, "isLLC");
	SescConf->isBool(section, "isL1");

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
  // predicated ARM instructions can be with zero address
  if(mreq->getAddr() == 0) {
    mreq->ack(tagDelay + dataDelay + goUpDelay);
    return;
  }
	I(!mreq->isRetrying());
	// [sizhuo] no contention
	mreq->redoReqAbs(globalClock);
}

void ACache::reqAck(MemRequest *mreq)
{
	I(!mreq->isRetrying());
	// [sizhuo] no contention
	mreq->redoReqAckAbs(globalClock);
}

void ACache::setState(MemRequest *mreq)
{
	I(!mreq->isRetrying());
	// [sizhuo] no contention
	mreq->redoSetStateAbs(globalClock);
}

void ACache::setStateAck(MemRequest *mreq)
{
	I(!mreq->isRetrying());
	// [sizhuo] no contention
	mreq->redoSetStateAckAbs(globalClock);
}

void ACache::disp(MemRequest *mreq)
{
	I(!mreq->isRetrying());
	// [sizhuo] no contention
	mreq->redoDispAbs(globalClock);
}

void ACache::doReq(MemRequest *mreq) {
	ID(mreq->dump("doReq"));
	// [sizhuo] forward req to lower level, wait ReqAck and complete req at doReqAck
	router->scheduleReq(mreq, tagDelay + goDownDelay);
}

void ACache::doReqAck(MemRequest *mreq) {
	ID(mreq->dump("doReqAck"));

	if(mreq->isHomeNode()) {
		// [sizhuo] this is home node, end the msg
		I(isL1); // [sizhuo] must be L1$
		mreq->ack(dataDelay + goUpDelay);
		return;
	} 

	// [sizhuo] this cache cannot be L1$
	I(!isL1);
	if(isL1) MSG("ERROR: req ack reaches L1$ but not home node");

	if(mreq->isRetrying()) {
		// [sizhuo] second time handling req ack
		// downgrade req has been done, resp to upper level
		mreq->clearRetrying();
		router->scheduleReqAck(mreq, goUpDelay);
		return;
	}

	// [sizhuo] broadcast to upper level except for the one with home node
	int32_t nmsg = router->sendSetStateOthers(mreq, ma_setInvalid, tagDelay + goUpDelay);

	if(nmsg > 0) {
		I(mreq->hasPendingSetStateAck());
		// [sizhuo] need to wait for downgrade resp to handle req ack again
		mreq->setRetrying();
	} else {
		I(!mreq->hasPendingSetStateAck());
		I(!mreq->isRetrying());
		// [sizhuo] no broadcast is made, just resp
		router->scheduleReqAck(mreq, goUpDelay);
	}
}

void ACache::doSetState(MemRequest *mreq) {
  I(!mreq->isHomeNode()); // [sizhuo] home node should be at lower level
	ID(mreq->dump("doSetState"));
	
	if(mreq->isRetrying()) {
		// [sizhuo] second time handling set state req
		// upper level already downgraded, just resp
		mreq->clearRetrying();
		mreq->convert2SetStateAck(ma_setInvalid);
		router->scheduleSetStateAck(mreq, goDownDelay);
		return;
	}

	// [sizhuo] broadcast to all upper level
	int32_t nmsg = router->sendSetStateAll(mreq, mreq->getAction(), tagDelay + goUpDelay);

	if(nmsg > 0) {
		I(mreq->hasPendingSetStateAck());
		// [sizhuo] need to wait for downgrade resp to handle req ack again
		mreq->setRetrying();
	} else {
		I(!mreq->hasPendingSetStateAck());
		I(!mreq->isRetrying());
		// [sizhuo] no broadcast is made, just resp
		mreq->convert2SetStateAck(ma_setInvalid);
		router->scheduleSetStateAck(mreq, goDownDelay);
	}
}

void ACache::doSetStateAck(MemRequest *mreq) {
	I(mreq->isHomeNode()); // [sizhuo] we must be at home node now
	GMSG(!mreq->isHomeNode(), "ERROR: SetStateAck arrives non-home node!");
	ID(mreq->dump("doSetStateAck"));

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
}

void ACache::doDisp(MemRequest *mreq) {
	ID(mreq->dump("doDisp"));
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
