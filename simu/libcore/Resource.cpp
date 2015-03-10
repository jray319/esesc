//  Contributed by Jose Renau
//                 Basilio Fraguela
//                 James Tuck
//                 Smruti Sarangi
//                 Luis Ceze
//                 Karin Strauss
//                 David Munday
//
// The ESESC/BSD License
//
// Copyright (c) 2005-2013, Regents of the University of California and 
// the ESESC Project.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   - Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
//
//   - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
//   - Neither the name of the University of California, Santa Cruz nor the
//   names of its contributors may be used to endorse or promote products
//   derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <limits.h>

#include "Resource.h"

#include "Cluster.h"
#include "DInst.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "GProcessor.h"
#include "OoOProcessor.h"
#include "MemRequest.h"
#include "Port.h"
#include "LSQ.h"

#define SCOORE_LSQ 0

/* }}} */

Resource::Resource(Cluster *cls, PortGeneric *aGen, TimeDelta_t l)
  /* constructor {{{1 */
  : cluster(cls)
  ,gen(aGen)
  ,gproc(cls->getGProcessor()) 
  ,lat(l)
  ,usedTime(0)
{
  I(cls);
  if(gen)
    gen->subscribe();
}
/* }}} */

Resource::~Resource()
/* destructor {{{1 */
{
  GMSG(!EventScheduler::empty(), "Resources destroyed with %zu pending instructions"
       ,EventScheduler::size());
  
  if(gen)
    gen->unsubscribe();
}
/* }}} */

void Resource::select(DInst *dinst)
/* callback entry point for select {{{1 */
{
  cluster->select(dinst);
}
/* }}} */

/***********************************************/

MemResource::MemResource(Cluster *cls, PortGeneric *aGen, LSQ *_lsq, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad)
  /* constructor {{{1 */
  : MemReplay(cls, aGen, ss, l)
  ,DL1(ms->getDL1())
  ,memorySystem(ms)
  ,lsq(_lsq)
  ,stldViolations("P(%d)_%s:stldViolations", id, cad)
{
}
/* }}} */

MemResource_noMemSpec::MemResource_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad)
  /* constructor {{{1 */
  : Resource(cls, aGen, l)
  ,DL1(ms->getDL1())
  ,memorySystem(ms)
{
}


SCOOREMem::SCOOREMem(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad)
  /* constructor {{{1 */
  : MemReplay(cls, aGen, ss, l)
  ,DL1(ms->getDL1())
  ,vpc(ms->getvpc())
  ,memorySystem(ms)
    //,ScooreUpperHierReplays("P(%d)_%s:ScooreUpperHierReplays", id, cad)
  ,enableDcache(SescConf->getBool("cpusimu", "enableDcache", id))
{
  
  if(vpc) {
    //printf("DEBUG: getting VPC delay\n");
    if(SescConf->checkInt(vpc->getSection(), "hitDelay"))
      vpcDelay = SescConf->getInt(vpc->getSection(), "hitDelay");
    else
      vpcDelay = SescConf->getInt(vpc->getSection(), "delay");
  }else{
    MSG("ERROR no VPC defined\n");
    exit(1);
  }

  if( SescConf->checkInt(DL1->getSection(), "hitDelay") )
    DL1Delay = SescConf->getInt(DL1->getSection(), "hitDelay");
  else
    DL1Delay = SescConf->getInt(DL1->getSection(), "delay");
}
/* }}} */

/***********************************************/

FUSCOORELoad::FUSCOORELoad(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad)
  /* constructor {{{1 */
  : SCOOREMem(cls, aGen, ss, l, ms, id, cad) {
  //printf("DEBUG: calling FUSCOORELoad constructor\n");
  I(ms);
  I(DL1);
  I(vpc);
  //#if SCOORE_LSQ
#if 0
  maxLoads=SescConf->getInt("cpusimu", "maxLoads",gproc->getId());
  free_entries = maxLoads;
#endif
}
/* }}} */

void FUSCOORELoad::retryvpc(DInst *dinst)
/* retry vpc request {{{1 */
{

  if (!vpc->isBusy(dinst->getAddr())) {
    MemRequest::sendReqRead(vpc, dinst, dinst->getAddr(), executedCB::create(this, dinst));
    return; 
  }

  retryvpcCB::schedule(7, this, dinst);
}
/* }}} */

#if 1
#define FIXSIZE (32768-1)
static uint32_t fixhash(AddrType addr) {
  return (addr>>2) & FIXSIZE;
}

DataType fixdata[FIXSIZE+1]; // FIXME: This only works single threaded
AddrType fixaddr[FIXSIZE+1]; // FIXME: This only works single threaded
#endif

StallCause FUSCOORELoad::canIssue(DInst *dinst) {
  /* canIssue {{{1 */

  //#if SCOORE_LSQ
#if 0
  if(free_entries <= 0){
    I(free_entries == 0); // Can't be negative
    return OutsLoadsStall;
  }
#endif
  

#if 0
  if (fixaddr[fixhash(dinst->getAddr())] == dinst->getAddr()) {
    DataType data = fixdata[fixhash(dinst->getAddr())];
    //I(data == dinst->getData());
    dinst->setData(data);
  }
#endif

  //storeset->insert(dinst); 
  //#if SCOORE_LSQ
#if 0
  free_entries--;
#endif
  return NoStall;
}
/* }}} */

void FUSCOORELoad::executing(DInst *dinst) {
  /* executing {{{1 */

  cluster->executing(dinst);

  //printf("DEBUG calling Loadexecuting:VPC\n");
  if(!enableDcache) {
    Time_t when = gen->nextSlot(dinst->getStatsFlag())+vpcDelay;
    executedCB::scheduleAbs(when, this, dinst);
    return;
  }    
  retryvpc(dinst);
}
/* }}} */

MemReplay::MemReplay(Cluster *cls, PortGeneric *gen, StoreSet *ss, TimeDelta_t l)
  :Resource(cls, gen, l)
   ,lfSize(8)
   ,storeset(ss)
{
  lf = (struct FailType *)malloc(sizeof(struct FailType)*lfSize);
  for(uint32_t i=0;i<lfSize;i++) {
    lf[i].ssid = -1;
  }
}

void MemReplay::replayManage(DInst* dinst) {

  if (dinst->getSSID()==-1)
    return;

  int pos = -1;
  int pos2 = 0;
  bool updated = false;

  SSID_t did = dinst->getSSID();

  for(uint32_t i=0;i<lfSize;i++) {
    if (lf[pos2].id < lf[i].id && lf[i].ssid != did)
      pos2 = i;

    if (lf[i].id >= dinst->getID())
      continue;
    if (lf[i].ssid == -1) {
      if (!updated) {
        lf[i].ssid = did;
        lf[i].id   = dinst->getID();
        lf[i].pc   = dinst->getPC();
        lf[i].addr = dinst->getAddr();
        lf[i].op   = dinst->getInst()->getOpcode();
        updated    = true;
      }
      continue;
    }
    if ((dinst->getID() - lf[i].id) > 500) {
      if (!updated) {
        lf[i].ssid = did;
        lf[i].id   = dinst->getID();
        lf[i].pc   = dinst->getPC();
        lf[i].addr = dinst->getAddr();
        lf[i].op   = dinst->getInst()->getOpcode();
        updated    = true;
      }
      continue;
    }
    if (lf[i].ssid == did) {
      continue;
    }
    if (lf[i].addr != dinst->getAddr()) {
      if (pos == -1)
        pos = i;
      continue;
    }

    if (pos!=-3) {
      pos = -2;
      if (lf[i].pc == dinst->getPC()) {
      }
#if 0
      printf("1.merging %d and %d : pc %x and %x : addr %x and %x : %dx%d : data %x and %x : id %d and %d (%d) : %x\n"
             ,lf[i].ssid, did, lf[i].pc, dinst->getPC(), lf[i].addr, dinst->getAddr()
             ,lf[i].op, dinst->getInst()->getOpcode()
             ,lf[i].data, dinst->getData(), lf[i].id, dinst->getID(), dinst->getID() - lf[i].id, dinst->getConflictStorePC());
#endif

      SSID_t newid = storeset->mergeset(lf[i].ssid, did);
      did = newid;
      lf[i].ssid = newid;
      lf[i].id   = dinst->getID();
      lf[i].pc   = dinst->getPC();
      lf[i].addr = dinst->getAddr();
      lf[i].op   = dinst->getInst()->getOpcode();
      updated    = true;
    }
  }

  static int rand = 0;
  rand++;
#if 1
  if (pos>=0 && (rand & 7) == 0) {
    int i = pos;
#if 0
      printf("2.merging %d and %d : pc %x and %x : addr %x and %x : %dx%d : data %x and %x : id %d and %d (%d) : %x\n"
          ,lf[i].ssid, dinst->getSSID(), lf[i].pc, dinst->getPC(), lf[i].addr, dinst->getAddr()
          , lf[i].op, dinst->getInst()->getOpcode()
          , lf[i].data, dinst->getData(), lf[i].id, dinst->getID(), dinst->getID() - lf[i].id, dinst->getConflictStorePC());
#endif
    SSID_t newid = storeset->mergeset(lf[i].ssid, dinst->getSSID()); 
    lf[i].ssid = newid;
    lf[i].id   = dinst->getID();
    lf[i].pc   = dinst->getPC();
    lf[i].addr = dinst->getAddr();
    updated    = true;
  }
#endif

  if (!updated) {
    int i = pos2;
    if (dinst->getID() > lf[i].id && dinst->getSSID() != lf[i].ssid) {
#if DEBUG
      printf("3.merging %d and %d : pc %lx and %lx : addr %lx and %lx : id %lu and %lu (%lu)\n",lf[i].ssid, dinst->getSSID(), lf[i].pc, dinst->getPC(), lf[i].addr, dinst->getAddr(), lf[i].id, dinst->getID(), dinst->getID() - lf[i].id);
      storeset->mergeset(lf[i].ssid, dinst->getSSID()); 
      if (lf[i].ssid > dinst->getSSID())
        lf[i].ssid = dinst->getSSID();
#endif
      lf[i].ssid = dinst->getSSID();
      lf[i].id   = dinst->getID();
      lf[i].pc   = dinst->getPC();
      lf[i].addr = dinst->getAddr();
    }
  }
}


void FUSCOORELoad::executed(DInst* dinst) {
  /* executed {{{1 */

  storeset->remove(dinst); 

  if(dinst->isReplay())  {
    //gproc->replay(dinst); called in VPC
    storeset->stldViolation(dinst, dinst->getConflictStorePC());
    //replayManage(dinst);
  }

  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FUSCOORELoad::preretire(DInst *dinst, bool flushing)
  /* preretire {{{1 */ {
  //printf("DEBUG: calling ScooreLoad::preretire\n");

  if (flushing) { 
    performed(dinst);
    return true;
  }

  if(!enableDcache) {
    performedCB::schedule(DL1Delay, this, dinst);
    return true;
  }
        
  if(DL1->isBusy(dinst->getAddr()))
    return false;
        
  MemRequest::sendReqRead(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));
  
  return true;
}
/* }}} */

bool FUSCOORELoad::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  //printf("DEBUG: calling ScooreLoad::retire\n");
  //#if SCOORE_LSQ
#if 0
  free_entries++;
#endif
  if(!dinst->isPerformed()) // FIXME? && !flushing)
    return false;

#if 1
  if(dinst->isReplay() && !flushing) {
    vpc->replayflush();
    replayManage(dinst);
  }
#endif

  return true;
}
/* }}} */

void FUSCOORELoad::performed(DInst *dinst) {
  //printf("DEBUG: calling ScooreLoad::performed\n");
  /* memory operation was globally performed {{{1 */
  if (dinst->isExecuted()) {
    // In-order RETIRE OPERATION
    dinst->markPerformed();
  }else{
    // Out-of-order EXECUTE OPERATION
    executed(dinst);
  }
}
/* }}} */

/***********************************************/

FUSCOOREStore::FUSCOOREStore(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad)
  /* Constructor {{{1 */
  : SCOOREMem(cls, aGen, ss, l, ms, id, cad) {
  I(ms);
  I(DL1);
  I(vpc);
  //#if SCOORE_LSQ
#if 0 
  maxStores = SescConf->getInt("cpusimu", "maxStores",gproc->getId());
  free_entries = maxStores;
#endif
}
/* }}} */


StallCause FUSCOOREStore::canIssue(DInst *dinst) {
  /* canIssue? {{{1 */

  if(dinst->getInst()->isStoreAddress())
    return NoStall;

  //#if SCOORE_LSQ
#if 0
  if(free_entries <= 0) {
    I(free_entries == ); //Can't be negative
    return OutsStoresStall;
  }
#endif

#if 1
  fixaddr[fixhash(dinst->getAddr())] = dinst->getAddr();
#endif
  
  //storeset->insert(dinst);
  //#if SCOORE_LSQ
#if 0
  free_entries--;
#endif

  return NoStall;
}
/* }}} */

void FUSCOOREStore::retryvpc(DInst *dinst)
/* retry vpc request {{{1 */
{
  I(dinst->getInst()->isStoreAddress() || dinst->getInst()->isStore());

  if (vpc->isBusy(dinst->getAddr())) {
    retryvpcCB::schedule(3, this, dinst);
    return;
  }

  if (dinst->getInst()->isStoreAddress()) {
    MemRequest::sendReqWritePrefetch(vpc, dinst, dinst->getAddr(), executedCB::create(this,dinst));
  }else{
    MemRequest::sendReqWrite(vpc, dinst, dinst->getAddr(), performedCB::create(this,dinst));
  }
}
/* }}} */

void FUSCOOREStore::executing(DInst *dinst) {
  /* executing {{{1 */
  cluster->executing(dinst);

  I(vpc);
  //printf("DEBUG Storeexecuting::VPC\n");
  if(!enableDcache) {
    Time_t when = gen->nextSlot(dinst->getStatsFlag())+vpcDelay;
    executedCB::scheduleAbs(when, this, dinst);
    return;
  }

  retryvpc(dinst);
}
/* }}} */

void FUSCOOREStore::executed(DInst *dinst) {
  //printf("DEBUG: calling ScooreStore::executed\n"); 
  /* executed {{{1 */
  if (dinst->getInst()->isStore()) {
    storeset->remove(dinst);
  }

  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FUSCOOREStore::preretire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  //printf("DEBUG: calling ScooreStore::preretire\n");
  if(dinst->getInst()->isStoreAddress())
    return true;

  I(dinst->getInst()->isStore());

  if(!enableDcache) {
    performedCB::schedule(DL1Delay, this, dinst);
    return true;
  }
  if (flushing) { //should we add a check for if(replay_recovering) here also?
    performed(dinst);
    return true;
  }

  if(DL1->isBusy(dinst->getAddr()))
    return false;

  vpc->replayCheckLSQ_removeStore(dinst);

  vpc->updateXCoreStores(dinst->getAddr());

	I(0); // FIXME
//  MemRequest::createVPCWriteUpdate(vpc, dinst); //no callback needed
  MemRequest::sendReqWrite(vpc, dinst, dinst->getAddr());

  MemRequest::sendReqWrite(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst)); 

  return true;
}
/* }}} */

bool FUSCOOREStore::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  //printf("DEBUG: calling ScooreStore::retire\n");
  if(dinst->getInst()->getOpcode() == iSALU_ADDR)
    return true;

  if(!dinst->isPerformed())
    return false;

  if(dinst->isReplay() && !flushing) {
    vpc->replayflush();
    replayManage(dinst);
  }

  //#if SCOORE_LSQ
#if 0
  free_entries++;
#endif

  return true;
}
/* }}} */

void FUSCOOREStore::performed(DInst *dinst)
/* memory operation was globally performed {{{1 */
{
  //printf("DEBUG: calling ScooreStore::performed\n");
  if (dinst->isExecuted()) {
    // In-order RETIRE OPERATION
    dinst->markPerformed();
  }else{
    // Out-of-order EXECUTE OPERATION
    executed(dinst);
  }
}
/* }}} */
/***********************************************/

FULoad::FULoad(Cluster *cls, PortGeneric *aGen, LSQ *_lsq, StoreSet *ss, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad)
  /* Constructor {{{1 */
  : MemResource(cls, aGen, _lsq, ss, l, ms, id, cad)
  ,LSDelay(lsdelay)
  ,freeEntries(size) {
  char cadena[1000];
  sprintf(cadena,"P(%d)_%s", id, cad);
  enableDcache = SescConf->getBool("cpusimu", "enableDcache", id);  
  I(ms);
}
/* }}} */

StallCause FULoad::canIssue(DInst *dinst) {
  /* canIssue {{{1 */

  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsLoadsStall;
  }
  storeset->insert(dinst); 

  lsq->insert(dinst);
  freeEntries--;
  return NoStall;
}
/* }}} */

void FULoad::executing(DInst *dinst) {
  /* executing {{{1 */

  cluster->executing(dinst);
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

  DInst *qdinst = lsq->executing(dinst);
  I(qdinst==0);
  if (qdinst) { // [sizhuo] should not enter here! qdinst should be 0
    I(qdinst->getInst()->isStore());
    gproc->replay(dinst);
    if(!gproc->isFlushing())
      stldViolations.inc(dinst->getStatsFlag());

    storeset->stldViolation(qdinst,dinst);
  }

  // The check in the LD Queue is performed always, for hit & miss
  if (dinst->isLoadForwarded() || !enableDcache) {
    executedCB::scheduleAbs(when+LSDelay, this, dinst);
  }else{
    cacheDispatchedCB::scheduleAbs(when, this, dinst);
  }
}
/* }}} */

void FULoad::cacheDispatched(DInst *dinst) {
  /* cacheDispatched {{{1 */
 
  I(enableDcache);
  I(!dinst->isLoadForwarded());

  if(DL1->isBusy(dinst->getAddr())) {
    Time_t when = gen->nextSlot(dinst->getStatsFlag());
    cacheDispatchedCB::scheduleAbs(when+7, this, dinst); //try again later
    return;
  }

  MemRequest::sendReqRead(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));
}
/* }}} */

void FULoad::executed(DInst* dinst) {
  /* executed {{{1 */
  storeset->remove(dinst); 

  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FULoad::preretire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

bool FULoad::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  freeEntries++;

  lsq->remove(dinst);

#if 0
  // Merging for tradcore
  if(dinst->isReplay() && !flushing) 
    replayManage(dinst);
#endif

  return true;
}
/* }}} */

void FULoad::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */

  dinst->markPerformed();

  // For LDs, a perform means finish the executed
  //executedCB::schedule(0);
  executed(dinst);
}
/* }}} */

/***********************************************/

FUStore::FUStore(Cluster *cls, PortGeneric *aGen, LSQ *_lsq, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad)
  /* constructor {{{1 */
  : MemResource(cls, aGen, _lsq, ss, l, ms, id, cad)
  ,freeEntries(size) {
  enableDcache = SescConf->getBool("cpusimu", "enableDcache", id);
}
/* }}} */

StallCause FUStore::canIssue(DInst *dinst) {
  /* canIssue {{{1 */ 

  if (dinst->getInst()->isStoreAddress())
    return NoStall;

  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsStoresStall;
  }
  
  storeset->insert(dinst);
  
  lsq->insert(dinst);

  freeEntries--;

  return NoStall;
}
/* }}} */

void FUStore::executing(DInst *dinst) {
  /* executing {{{1 */
  if (!dinst->getInst()->isStoreAddress()) {
    DInst *qdinst = lsq->executing(dinst);
    if (qdinst) {
      gproc->replay(qdinst);
      if(!gproc->isFlushing())
        stldViolations.inc(dinst->getStatsFlag());
      storeset->stldViolation(qdinst,dinst);
    }
  }

  cluster->executing(dinst);
  gen->nextSlot(dinst->getStatsFlag());

  if (dinst->getInst()->isStoreAddress()) {
#if 1
    if (enableDcache)
      MemRequest::sendReqWritePrefetch(DL1, dinst, dinst->getAddr(), executedCB::create(this,dinst));
    else
      executed(dinst);
#else
    executed(dinst);
#endif
  }else{
    executed(dinst);
  }
}
/* }}} */

void FUStore::executed(DInst *dinst) {
  /* executed {{{1 */

  if (dinst->getInst()->isStore())
    storeset->remove(dinst);

  dinst->markExecuted();
  cluster->executed(dinst);
  // We have data&address, the LSQ can be populated
}
/* }}} */

bool FUStore::preretire(DInst *dinst, bool flushing) {
  /* retire {{{1 */
  if (dinst->getInst()->isStoreAddress())
    return true;

  if (flushing) {
    performed(dinst);
    return true;
  }
  if (!enableDcache) {
    if (dinst->isExecuted()) {
      performed(dinst);
      return true;
    }
    return false;
  }

  if(DL1->isBusy(dinst->getAddr()) ) {
    return false;
  }

  MemRequest::sendReqWrite(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));

  return true;
}
/* }}} */

void FUStore::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */
  dinst->markPerformed();
}
/* }}} */

bool FUStore::retire(DInst *dinst, bool flushing) {
  /* retire {{{1 */
  if(dinst->getInst()->isStoreAddress())
    return true;

	// [sizhuo] FIXME: store should be able to retire before globally performed
  if(!dinst->isPerformed())
    return false;

  lsq->remove(dinst);
  freeEntries++;

  return true;
}
/* }}} */

/***********************************************/

FUGeneric::FUGeneric(Cluster *cls ,PortGeneric *aGen ,TimeDelta_t l )
  /* constructor {{{1 */
  :Resource(cls, aGen, l)
   {
}
/* }}} */

StallCause FUGeneric::canIssue(DInst *dinst) {
  /* canIssue {{{1 */
  return NoStall;
}
/* }}} */

void FUGeneric::executing(DInst *dinst) {
  /* executing {{{1 */
  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}
/* }}} */

void FUGeneric::executed(DInst *dinst) {
  /* executed {{{1 */
  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FUGeneric::preretire(DInst *dinst, bool flushing)
/* preretire {{{1 */
{
  return true;
}
/* }}} */

bool FUGeneric::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

void FUGeneric::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */
  dinst->markPerformed();
  I(0); // It should be called only for memory operations
}
/* }}} */

/***********************************************/

FUFuze::FUFuze(Cluster *cls ,PortGeneric *aGen ,TimeDelta_t l )
  /* constructor {{{1 */
  :Resource(cls, aGen, l)
   {
}
/* }}} */

StallCause FUFuze::canIssue(DInst *dinst) {
  /* canIssue {{{1 */
  return NoStall;
}
/* }}} */

void FUFuze::executing(DInst *dinst) {
  /* executing {{{1 */
  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}
/* }}} */

void FUFuze::executed(DInst *dinst) {
  /* executed {{{1 */
  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FUFuze::preretire(DInst *dinst, bool flushing)
/* preretire {{{1 */
{
  return true;
}
/* }}} */

bool FUFuze::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

void FUFuze::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */
  dinst->markPerformed();
  I(0); // It should be called only for memory operations
}
/* }}} */

/***********************************************/

FUBranch::FUBranch(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, int32_t mb)
  /* constructor {{{1 */
  :Resource(cls, aGen, l)
  ,freeBranches(mb) {
  I(freeBranches>0);
}
/* }}} */

StallCause FUBranch::canIssue(DInst *dinst) {
  /* canIssue {{{1 */
  if (freeBranches == 0)
    return OutsBranchesStall;

  freeBranches--;

  return NoStall;
}
/* }}} */

void FUBranch::executing(DInst *dinst) {
  /* executing {{{1 */
  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);
}
/* }}} */

void FUBranch::executed(DInst *dinst) {
  /* executed {{{1 */
  dinst->markExecuted();
  cluster->executed(dinst);

  if (dinst->getFetch()) { // [sizhuo] branch resolves, resume fetch engine
    (dinst->getFetch())->unBlockFetch(dinst, dinst->getFetchTime());
    //IS(dinst->setFetch(0));
    IS(dinst->lockFetch(0));
  }

  // NOTE: assuming that once the branch is executed the entry can be recycled
  freeBranches++;
}
/* }}} */

bool FUBranch::preretire(DInst *dinst, bool flushing)
/* preretire {{{1 */
{
  return true;
}
/* }}} */

bool FUBranch::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

void FUBranch::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */
  dinst->markPerformed();
  I(0); // It should be called only for memory operations
}
/* }}} */

/***********************************************/

FURALU::FURALU(Cluster *cls ,PortGeneric *aGen ,TimeDelta_t l, bool scooreMemory_, int32_t id)
  /* constructor {{{1 */
  :Resource(cls, aGen, l)
  ,memoryBarrier("P(%d)_memoryBarrier",id)
{
  blockUntil = 0;
  scooreMemory = scooreMemory_;
}
/* }}} */

StallCause FURALU::canIssue(DInst *dinst) 
/* canIssue {{{1 */
{
  I(dinst->getPC() != 0xf00df00d); // It used to be a Syspend, but not longer true

  if (dinst->getPC() == 0xdeaddead){ 
    // This is the PC for a syscall (QEMUReader::syscall)
    if (blockUntil==0) {
      //LOG("syscall %d executed, with %d delay", dinst->getAddr(), dinst->getData());
      //blockUntil = globalClock+dinst->getData();
      blockUntil = globalClock+100;
      return SyscallStall;
    }
    
    //is this where we poweron the GPU threads and then poweroff the QEMU thread?
    if (globalClock >= blockUntil) {
      blockUntil = 0;
      return NoStall;
    }
    
    return SyscallStall;
  }else if (!dinst->getInst()->hasDstRegister() 
            && !dinst->getInst()->hasSrc1Register() 
            && !dinst->getInst()->hasSrc2Register()
            && !scooreMemory) {
    if (gproc->isROBEmpty())
      return NoStall;
    memoryBarrier.inc(dinst->getStatsFlag());
    return SyscallStall;
  }
  
  return NoStall;
}
/* }}} */

void FURALU::executing(DInst *dinst) 
/* executing {{{1 */
{
  cluster->executing(dinst);
  executedCB::scheduleAbs(gen->nextSlot(dinst->getStatsFlag())+lat, this, dinst);

  //Recommended poweron the GPU threads and then poweroff the QEMU thread?
}
/* }}} */

void FURALU::executed(DInst *dinst) 
/* executed {{{1 */
{
  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FURALU::preretire(DInst *dinst, bool flushing)
/* preretire {{{1 */
{
  return true;
}
/* }}} */

bool FURALU::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

void FURALU::performed(DInst *dinst) 
/* memory operation was globally performed {{{1 */
{
  dinst->markPerformed();
  I(0); // It should be called only for memory operations
}
/* }}} */

/*********UNITS WITH NO MEMORY SPECULATION NEXT********************/

FULoad_noMemSpec::FULoad_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad)
  /* Constructor {{{1 */
  : MemResource_noMemSpec(cls, aGen, l, ms, id, cad)
  ,LSDelay(lsdelay)
  ,freeEntries(size) {
  char cadena[1000];
  sprintf(cadena,"P(%d)_%s", id, cad);
  enableDcache = SescConf->getBool("cpusimu", "enableDcache", id);  
  I(ms);
}
/* }}} */

StallCause FULoad_noMemSpec::canIssue(DInst *dinst) {
  /* canIssue {{{1 */
  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsLoadsStall;
  }
  freeEntries--;
  return NoStall;
}
/* }}} */

void FULoad_noMemSpec::executing(DInst *dinst) {
  /* executing {{{1 */

  cluster->executing(dinst);
  Time_t when = gen->nextSlot(dinst->getStatsFlag())+lat;

  // The check in the LD Queue is performed always, for hit & miss
  if (dinst->isLoadForwarded() || !enableDcache) {
    executedCB::scheduleAbs(when+LSDelay, this, dinst);
  }else{
    cacheDispatchedCB::scheduleAbs(when, this, dinst);
  }
}
/* }}} */

void FULoad_noMemSpec::cacheDispatched(DInst *dinst) {
  /* cacheDispatched {{{1 */
 
  I(enableDcache);
  I(!dinst->isLoadForwarded());

  if(DL1->isBusy(dinst->getAddr())) {
    Time_t when = gen->nextSlot(dinst->getStatsFlag());
    cacheDispatchedCB::scheduleAbs(when+7, this, dinst); //try again later
    return;
  }

  MemRequest::sendReqRead(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));
}
/* }}} */

void FULoad_noMemSpec::executed(DInst* dinst) {
  /* executed {{{1 */
  dinst->markExecuted();
  cluster->executed(dinst);
}
/* }}} */

bool FULoad_noMemSpec::preretire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  return true;
}
/* }}} */

bool FULoad_noMemSpec::retire(DInst *dinst, bool flushing)
/* retire {{{1 */
{
  freeEntries++;
  return true;
}
/* }}} */

void FULoad_noMemSpec::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */

  dinst->markPerformed();

  // For LDs, a perform means finish the executed
  //executedCB::schedule(0);
  executed(dinst);
}
/* }}} */

/***********************************************/

FUStore_noMemSpec::FUStore_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad)
  /* constructor {{{1 */
  : MemResource_noMemSpec(cls, aGen, l, ms, id, cad)
  ,freeEntries(size) {
  enableDcache = SescConf->getBool("cpusimu", "enableDcache", id);
}
/* }}} */

StallCause FUStore_noMemSpec::canIssue(DInst *dinst) {
  /* canIssue {{{1 */ 

  if (dinst->getInst()->isStoreAddress())
    return NoStall;

  if( freeEntries <= 0 ) {
    I(freeEntries == 0); // Can't be negative
    return OutsStoresStall;
  }
  
  freeEntries--;
  return NoStall;
}
/* }}} */

void FUStore_noMemSpec::executing(DInst *dinst) {
  /* executing {{{1 */
  cluster->executing(dinst);
  gen->nextSlot(dinst->getStatsFlag());

  if (dinst->getInst()->isStoreAddress()) {
    MemRequest::sendReqWritePrefetch(DL1, dinst, dinst->getAddr(), executedCB::create(this,dinst));
  }else{
    executed(dinst);
  }
}
/* }}} */

void FUStore_noMemSpec::executed(DInst *dinst) {
  /* executed {{{1 */
  dinst->markExecuted();
  cluster->executed(dinst);
  // We have data&address, the LSQ can be populated
}
/* }}} */

void FUStore_noMemSpec::performed(DInst *dinst) {
  /* memory operation was globally performed {{{1 */
  dinst->markPerformed();
}
/* }}} */

bool FUStore_noMemSpec::preretire(DInst *dinst, bool flushing) {
  /* retire {{{1 */
  if (dinst->getInst()->isStoreAddress())
    return true;

  if (flushing) {
    performed(dinst);
    return true;
  }
  if (!enableDcache) {
    if (dinst->isExecuted()) {
      performed(dinst);
      return true;
    }
    return false;
  }

  if(DL1->isBusy(dinst->getAddr()) ) {
    return false;
  }

  MemRequest::sendReqWrite(DL1, dinst, dinst->getAddr(), performedCB::create(this,dinst));

  return true;
}
/* }}} */

bool FUStore_noMemSpec::retire(DInst *dinst, bool flushing) {
  /* retire {{{1 */
  if(dinst->getInst()->isStoreAddress())
    return true;

  if(!dinst->isPerformed())
    return false;
  freeEntries++;
  return true;
}
/* }}} */

/***********************************************/

