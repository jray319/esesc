#include "MTLSQ.h"
#include "MTStoreSet.h"
#include "GProcessor.h"
#include "SescConf.h"
#include "DInst.h"

WMMLSQ::WMMLSQ(GProcessor *gproc_, bool ldldOrder, bool ldstOrder)
  : MTLSQ(gproc_)
  , orderLdLd(ldldOrder)
  , orderLdSt(ldstOrder)
{
  MSG("INFO: create P(%d)_WMMLSQ, orderLdLd %d, orderLdSt %d, log2LineSize %u, maxLd %d, maxSt %d, prefetch %d",
      gproc->getId(), orderLdLd, orderLdSt, log2LineSize, maxLdNum, maxStNum, prefetch);
}

StallCause WMMLSQ::addEntry(DInst *dinst) {
  I(dinst);
  I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
  I(freeLdNum >= 0);
  I(freeStNum >= 0);
  I(freeLdNum <= maxLdNum);
  I(freeStNum <= maxStNum);
  const Instruction *const ins = dinst->getInst();
  I(ins->isLoad() || ins->isStore() || ins->isRecFence());

  // [sizhuo] check free entry & reduce free count
  if(ins->isLoad() || ins->isRecFence()) {
    if(freeLdNum == 0) {
      return OutsLoadsStall;
    }
    freeLdNum--;
    I(freeLdNum >= 0);
  } else if(ins->isStore()) {
    if(freeStNum == 0) {
      return OutsStoresStall;
    }
    freeStNum--;
    I(freeStNum >= 0);
  }

  if(ins->isRecFence()) {
    // [sizhuo] immediately insert reconcile fence into LSQ
    // to make sure no younger load misses it
    SpecLSQEntry *en = specLSQEntryPool.out();
    I(en);
    en->clear();
    en->dinst = dinst;
    en->state = Done;
    std::pair<SpecLSQ::iterator, bool> insertRes = specLSQ.insert(std::make_pair<Time_t, SpecLSQEntry*>(dinst->getID(), en));
    I(insertRes.second);
  } else if(ins->isLoad() || ins->isStore()) {
    // [sizhuo] insert a NULL ptr into LSQ as placeholder to stop store from entering comSQ
    std::pair<SpecLSQ::iterator, bool> insertRes = specLSQ.insert(std::make_pair<Time_t, SpecLSQEntry*>(dinst->getID(), 0));
    I(insertRes.second);
  }

  return NoStall;
}

void WMMLSQ::issue(DInst *dinst) {
  I(dinst);
  const Instruction *const ins = dinst->getInst();
  const Time_t id = dinst->getID();
  const AddrType addr = dinst->getAddr();
  I(ins->isLoad() || ins->isStore());

  // [sizhuo] catch poisoned inst
  if(dinst->isPoisoned()) {
    // [sizhuo] mark executed without inserting to LSQ
    I(!dinst->isExecuted());
    dinst->markExecuted();
    // [sizhuo] the NULL specLSQ entry should have been deleted in reset()
    // but we just check here for safety
    SpecLSQ::iterator iter = specLSQ.find(dinst->getID());
    if(iter != specLSQ.end()) {
      I(0);
      I(iter->second == 0);
      specLSQ.erase(iter);
    }
    return;
  }
  
  // [sizhuo] create new entry
  SpecLSQEntry *issueEn = specLSQEntryPool.out();
  issueEn->clear();
  issueEn->dinst = dinst;
  // [sizhuo] we have already inserted a NULL ptr into specLSQ
  // now just fill in entry
  SpecLSQ::iterator issueIter = specLSQ.find(id);
  I(issueIter != specLSQ.end());
  I(issueIter->first == id);
  I(issueIter->second == 0);
  issueIter->second = issueEn;

  // [sizhuo] search younger entry (with higer ID) to find eager load to kill
  // TODO: if we are clever enough, load can bypass from younger load instead of killing
  // store will always do the search, load will only do it only when orderLdLd == true
  if(ins->isStore() || orderLdLd) {
    SpecLSQ::iterator iter = issueIter;
    iter++;
    for(; iter != specLSQ.end(); iter++) {
      I(iter->first > id);
      SpecLSQEntry *killEn = iter->second;
      if(!killEn) { // [sizhuo] NULL entry, skip it
        continue;
      }
      DInst *killDInst = killEn->dinst;
      I(killDInst);
      if(killDInst->isPoisoned()) {
        // [sizhuo] can't be poisoned XXX I don't know why I have this comment before
        // anyway, we can stop the search
        break;
      }
      const Instruction *const killIns = killDInst->getInst();
      const bool doStats = killDInst->getStatsFlag();
      // [sizhuo] we hit a reconcile fence we can stop
      if(killIns->isRecFence()) {
        break;
      }
      // XXX: [sizhuo] search load to same ALIGNED address that reads across the issuing inst
      // XXX: we decide kill/re-ex based on load source ID
      if(getMemOrdAlignAddr(killDInst->getAddr()) == getMemOrdAlignAddr(addr)) {
        if(killIns->isLoad()) {
          if(killEn->state == Wait) {
            // [sizhuo] younger loads on same aligned addr cannot read across this one
            // they are either stalled or killed, so stop here
            break;
          } else if(killEn->state == Exe) {
            if(killEn->ldSrcID < id) {
              // [sizhuo] we don't need to kill it, just let it re-execute
              killEn->needReEx = true;
              // [sizhuo] don't add to store set
              // because SC impl only adds when ROB flush
              // [sizhuo] stats
              if(ins->isStore()) {
                nLdReExBySt.inc(doStats);
              } else {
                nLdReExByLd.inc(doStats);
              }
            }
            // [sizhuo] if we don't force Ld-Ld ordering, we should continue search
          } else if(killEn->state == Done) {
            if(killEn->ldSrcID < id) {
              // [sizhuo] this load must be killed & set replay reason
              // NOTE: 1 inst may be killed multiple times, the last kill wins
              killDInst->setReplayReason(ins->isStore() ? DInst::Store : DInst::Load);
              gproc->replay(killDInst);
              // [sizhuo] add to store set
              mtStoreSet->memDepViolate(dinst, killDInst);
              // [sizhuo] stats
              if(ins->isStore()) {
                nLdKillBySt.inc(doStats);
              } else {
                nLdKillByLd.inc(doStats);
              }
              break; // [sizhuo] ROB will flush, we can stop
            } else {
              // [sizhuo] if bypass & kill are at same alignment
              // then we can stop here
              // but generally we should continue the search
            }
          } else {
            I(0);
          }
        } else {
          I(killIns->isStore());
          // [sizhuo] if bypass & kill are at same alignment
          // then we can stop here
          // but generally we should continue the search
        }
      }
    }
  }

  if(ins->isLoad()) {
    // [sizhuo] send load to execution
    scheduleLdEx(dinst);
  } else if(ins->isStore()) {
    // [sizhuo] store is done, change state, inform resource
    issueEn->state = Done;
    // [sizhuo] call immediately, easy for flushing
    dinst->getClusterResource()->executed(dinst);
  } else {
    I(0);
  }
}

void WMMLSQ::ldExecute(DInst *dinst) {
  I(dinst);
  I(dinst->getInst()->isLoad());
  const AddrType addr = dinst->getAddr();
  const Time_t id = dinst->getID();
  const bool doStats = dinst->getStatsFlag();

  // [sizhuo] get execute entry
  SpecLSQ::iterator exIter = specLSQ.find(id);
  I(exIter != specLSQ.end());
  I(exIter->first == id);
  SpecLSQEntry *exEn = exIter->second;
  I(exEn);
  I(exEn->dinst == dinst);
  I(exEn->state == Wait);

  // [sizhuo] catch poisoned inst
  if(dinst->isPoisoned()) {
    removePoisonedEntry(exIter);
    return;
  }

  // [sizhuo] search older inst (with lower ID) in specLSQ for bypass or stall
  // XXX: decrement iterator smaller than begin() results in undefined behavior
  SpecLSQ::iterator iter = exIter;
  while(iter != specLSQ.begin()) {
    iter--;
    I(iter != specLSQ.end());
    SpecLSQEntry *olderEn = iter->second;
    if(!olderEn) { // [sizhuo] skip NULL entry
      continue;
    }
    DInst *olderDInst = olderEn->dinst;
    I(olderDInst);
    const Instruction *const olderIns = olderDInst->getInst();
    I(!olderDInst->isPoisoned()); // [sizhuo] can't be poisoned
    I(iter->first < id);
    // [sizhuo] we hit reconcile fence, we should should stall until it retires
    if(olderIns->isRecFence()) {
      // [sizhuo] add event to pendRetireQ
      (olderEn->pendRetireQ).push(scheduleLdExCB::create(this, dinst));
      // [sizhuo] stats
      nLdStallByRec.inc(doStats);
      return;
    }
    // [sizhuo] we find inst to same ALIGNED address for bypass or stall
    if(getMemOrdAlignAddr(olderDInst->getAddr()) == getMemOrdAlignAddr(addr)) {
      if(olderIns->isLoad()) {
        if(olderEn->state == Done) {
          // [sizhuo] we can bypass from this executed load, mark as executing
          exEn->ldSrcID = olderEn->ldSrcID; // [sizhuo] same load src ID
          exEn->state = Exe;
          incrExLdNum(); // [sizhuo] increment executing ld num
          // [sizhuo] finish the load after forwarding delay
          ldDoneCB::schedule(ldldForwardDelay, this, dinst);
          // [sizhuo] stats
          nLdLdForward.inc(doStats);
          return;
        } else if(olderEn->state == Wait || olderEn->state == Exe) {
          // [sizhuo] stall on this older load which has not finished
          (olderEn->pendExQ).push(scheduleLdExCB::create(this, dinst));
          // [sizhuo] don't add to store set
          // because SC impl only add to store set when ROB flush
          // [sizhuo] stats
          nLdStallByLd.inc(doStats);
          return;
        } else {
          I(0);
        }
      } else if(olderIns->isStore()) {
        // [sizhuo] we can bypass from this executed store
        // mark the entry as executing
        exEn->ldSrcID = olderDInst->getID(); // [sizhuo] record this store as load src
        exEn->state = Exe;
        incrExLdNum(); // [sizhuo] increment executing ld num
        // [sizhuo] finish the load after forwarding delay
        ldDoneCB::schedule(stldForwardDelay, this, dinst);
        // [sizhuo] stats
        nStLdForward.inc(doStats);
        return;
      } else {
        I(0);
      }
    }
  }

  // [sizhuo] search commited store queue for bypass (start from youngest, i.e. largest ID)
  for(ComSQ::reverse_iterator rIter = comSQ.rbegin(); rIter != comSQ.rend(); rIter++) {
    ComSQEntry *en = rIter->second;
    I(en);
    I(rIter->first < id);
    // [sizhuo] XXX: bypass from same ALIGNED address
    if(getMemOrdAlignAddr(en->addr) == getMemOrdAlignAddr(addr)) {
      // [sizhuo] bypass from commited store, set ld as executing
      exEn->ldSrcID = rIter->first; // [sizhuo] record this store as load src
      exEn->state = Exe;
      incrExLdNum(); // [sizhuo] increment executing ld num
      // [sizhuo] finish load after forwarding delay
      ldDoneCB::schedule(stldForwardDelay, this, dinst);
      // [sizhuo] stats
      nStLdForward.inc(doStats);
      return;
    }
  }

  // [sizhuo] now we need to truly send load to memory hierarchy
  exEn->state = Exe;
  incrExLdNum(); // [sizhuo] increment executing ld num
  exEn->ldSrcID = DInst::invalidID; // [sizhuo] load src is memory
  MemRequest::sendReqRead(DL1, dinst->getStatsFlag(), addr, ldDoneCB::create(this, dinst));
}

bool WMMLSQ::retire(DInst *dinst) {
  I(dinst);
  I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
  const Instruction *const ins = dinst->getInst();
  I(ins->isLoad() || ins->isStore() || ins->isRecFence() || ins->isComFence());

  // [sizhuo] for commit fence, just check comSQ empty
  if(ins->isComFence()) {
    return comSQ.empty();
  }

  // [sizhuo] for other inst, try to find the entry in spec LSQ
  SpecLSQ::iterator retireIter = specLSQ.find(dinst->getID());
  SpecLSQEntry *retireEn = 0;
  if(retireIter == specLSQ.end()) {
    // [sizhuo] for store, since we may retire it early, it may not in spec LSQ
    I(ins->isStore());
    // TODO try to find a younger store to retire
  } else {
    retireEn = retireIter->second; // [sizhuo] set valid retire entry
    I(retireIter == specLSQ.begin());
    I(retireIter->first == dinst->getID());
    I(retireEn);
    I(retireEn->dinst == dinst);
    I(retireEn->state == Done);
    // [sizhuo] pend ex Q must be empty
    I((retireEn->pendExQ).empty());
    // [sizhuo] load/store: pend retire Q must be empty
    GI(dinst->getInst()->isLoad() || dinst->getInst()->isStore(), (retireEn->pendRetireQ).empty());
  }

  // [sizhuo] retire from spec LSQ
  if(retireIter != specLSQ.end()) {
    specLSQ.erase(retireIter);
  }

  // [sizhuo] only free load entry (LD/RECONCILE)
  if(ins->isLoad() || ins->isRecFence()) {
    freeLdNum++;
    I(freeLdNum > 0);
    I(freeLdNum <= maxLdNum);
  }

  // [sizhuo] actions when retiring
  if(ins->isStore() && retireEn) {
    // XXX the retired specLSQ entry may be different from dinst
    // it could be some younger store, so get info from retireEn->dinst instead of dinst
    const Time_t id = retireEn->dinst->getID();
    const AddrType addr = retireEn->dinst->getAddr();
    const bool doStats = retireEn->dinst->getStatsFlag();
    // [sizhuo] insert to comSQ
    ComSQEntry *en = comSQEntryPool.out();
    en->clear();
    en->addr = addr;
    en->doStats = doStats;
    std::pair<ComSQ::iterator, bool> insertRes = comSQ.insert(std::make_pair<Time_t, ComSQEntry*>(id, en));
    I(insertRes.second);
    ComSQ::iterator comIter = insertRes.first;
    I(comIter != comSQ.end());
    I(en == (comSQ.rbegin())->second);
    // [sizhuo] XXX: if comSQ doesn't have older store to same CACHE LINE
    // send this one to memory
    bool noOlderSt = true;
    while(comIter != comSQ.begin()) {
      comIter--;
      I(comIter != comSQ.end());
      I(comIter->second);
      I(comIter->first < id);
      if(getLineAddr(comIter->second->addr) == getLineAddr(addr)) {
        noOlderSt = false;
        break;
      }
    }
    if(noOlderSt) {
      // [sizhuo] newly inserted store must be youngest, can't carry other stores to D$
      // only set exID of itself
      en->exID = id;
      stToMemCB::scheduleAbs(stToMemPort->nextSlot(doStats), this, addr, id, doStats);
    }
  } else if(ins->isRecFence()) {
    // [sizhuo] reconcile: call events in pend retireQ next cycle
    while(!(retireEn->pendRetireQ).empty()) {
      CallbackBase *cb = (retireEn->pendRetireQ).front();
      (retireEn->pendRetireQ).pop();
      cb->schedule(1);
    }
  } else if(ins->isLoad()) {
    // decrement done ld num
    doneLdNum--;
    I(doneLdNum >= 0);
  }

  // [sizhuo] recycle spec LSQ entry
  if(retireEn) {
    I((retireEn->pendRetireQ).empty() && (retireEn->pendExQ).empty());
    specLSQEntryPool.in(retireEn);
  }

  // [sizhuo] unalignment stats
  if(dinst->getAddr() & ((0x01ULL << memOrdAlignShift) - 1)) {
    const bool doStats = dinst->getStatsFlag();
    if(ins->isLoad()) {
      nUnalignLd.inc(doStats);
    } else if(ins->isStore()) {
      nUnalignSt.inc(doStats);
    }
  }

  // [sizhuo] retire success
  return true;
}

void WMMLSQ::stCommited(Time_t id) {
  // [sizhuo] find the entry with this id, i.e. the oldest store just commited to D$
  ComSQ::iterator comIter = comSQ.find(id);
  I(comIter != comSQ.end());
  ComSQEntry *comEn = comIter->second;
  I(comEn);
  I(comEn->exID == id);
  // [sizhuo] record the line addr
  const AddrType lineAddr = getLineAddr(comEn->addr);
  // [sizhuo] delete this entry & recycle it
  comSQ.erase(comIter);
  comSQEntryPool.in(comEn);
  // [sizhuo] increment free entry
  freeStNum++;
  I(freeStNum > 0);
  I(freeStNum <= maxStNum);

  // [sizhuo] delete all other stores just commited to D$ by identifying exID
  while(true) {
    ComSQ::iterator iter = comSQ.begin();
    for(; iter != comSQ.end(); iter++) {
      I(iter->second);
      if(iter->second->exID == id) {
        // [sizhuo] find the store to delete, break
        I(getLineAddr(iter->second->addr) == lineAddr);
        I(iter->first > id);
        break;
      }
    }
    if(iter != comSQ.end()) {
      // [sizhuo] we have store to delete & recycle & free
      ComSQEntry *en = iter->second;
      comSQ.erase(iter);
      comSQEntryPool.in(en);
      // [sizhuo] increment free entry
      freeStNum++;
      I(freeStNum > 0);
      I(freeStNum <= maxStNum);
    } else {
      // [sizhuo] no more store to delete, stop
      break;
    }
  }

  // [sizhuo] send the current oldest entry of this CACHE LINE to memory
  for(ComSQ::iterator exIter = comSQ.begin(); exIter != comSQ.end(); exIter++) {
    ComSQEntry *exEn = exIter->second;
    I(exEn);
    if(getLineAddr(exEn->addr) == lineAddr) {
      // [sizhuo] got the store to send to memory
      I(exIter->first > id);
      I(exEn->exID == DInst::invalidID);
      // [sizhuo] set exID field to the inst ID of itself
      const Time_t exStID = exIter->first;
      exEn->exID = exStID;
      // [sizhuo] set exID of all younger stores to same CACHE LINE
      ComSQ::iterator iter = exIter;
      iter++;
      for(; iter != comSQ.end(); iter++) {
        ComSQEntry *en = iter->second;
        I(en);
        if(getLineAddr(en->addr) == lineAddr) {
          I(en->exID == DInst::invalidID);
          en->exID = exStID;
        }
      }
      // [sizhuo] send store to memory
      stToMemCB::scheduleAbs(stToMemPort->nextSlot(exEn->doStats), this, exEn->addr, exStID, exEn->doStats);
      break;
    }
  }
}

void WMMLSQ::reset() {
  // [sizhuo] remove all reconcile & store & executed load & NULL entry
  while(true) {
    SpecLSQ::iterator rmIter = specLSQ.begin();
    // [sizhuo] search through specLSQ for reconcile & store & executed load
    // because these entries are only invoked by retire(), which will not be called in flushing mode
    for(; rmIter != specLSQ.end(); rmIter++) {
      SpecLSQEntry *rmEn = rmIter->second;
      if(!rmEn) {
        // [sizhuo] NULL entry, we need to erase it
        break;
      }
      I(rmEn->dinst);
      const Instruction *const rmIns = rmEn->dinst->getInst();
      I(rmIns);
      if(rmIns->isRecFence() || rmIns->isStore() || (rmIns->isLoad() && rmEn->state == Done)) {
        break;
      }
    }
    if(rmIter != specLSQ.end()) {
      // [sizhuo] find something to remove
      if(rmIter->second) {
        removePoisonedEntry(rmIter); // [sizhuo] non-NULL entry
      } else {
        specLSQ.erase(rmIter); // [sizhuo] NULL entry, directly remove
      }
    } else {
      // [sizhuo] no more to remove stop
      break;
    }
  }
  // [sizhuo] recover free entry num
  freeLdNum = maxLdNum;
  freeStNum = maxStNum - comSQ.size();
  I(freeStNum >= 0);
  // [sizhuo] recover stats vars
  exLdNum = 0;
  doneLdNum = 0;
}

