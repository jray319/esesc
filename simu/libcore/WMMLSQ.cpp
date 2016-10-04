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
  I(ins->isLoad() || ins->isStore() || ins->isRecFence() || ins->isComFence());

  // [sizhuo] check free entry & reduce free count
  if(ins->isLoad() || ins->isRecFence()) {
    if(freeLdNum == 0) {
      return OutsLoadsStall;
    }
    freeLdNum--;
    I(freeLdNum >= 0);
  } else if(ins->isStore() || ins->isComFence()) {
    if(freeStNum == 0) {
      return OutsStoresStall;
    }
    freeStNum--;
    I(freeStNum >= 0);
  } else {
    I(0);
  }

  if(ins->isRecFence() || ins->isComFence()) {
    // [sizhuo] immediately insert reconcile fence into LSQ
    // to make sure no younger load misses it
    // we also insert commit fence into LSQ for preventing store early retire
    // (although this is not required by mem model, this accelerates the retire of commit itself)
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
  } else {
    I(0);
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
        // [sizhuo] can't be poisoned
        // because we poison all instructions in ROB together when the replayed inst reaches ROB head
        // anyway, we can stop the search
        I(0);
        break;
      }
      const Instruction *const killIns = killDInst->getInst();
      const bool doStats = killDInst->getStatsFlag();
      // [sizhuo] we hit a reconcile fence we can stop
      if(killIns->isRecFence()) {
        break;
      }
      // [sizhuo] omit commit fence
      if(killIns->isComFence()) {
        continue;
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
    // [sizhuo] omit commit fence
    if(olderIns->isComFence()) {
      continue;
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
    // [sizhuo] XXX: bypass from same ALIGNED address
    if(getMemOrdAlignAddr(en->addr) == getMemOrdAlignAddr(addr)) {
      // [sizhuo] bypass from commited store, set ld as executing
      I(rIter->first < id);
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

void WMMLSQ::sendStToComSQ(MTLSQ::SpecLSQEntry *retireEn) {
  const Time_t id = retireEn->dinst->getID();
  const AddrType addr = retireEn->dinst->getAddr();
  const bool doStats = retireEn->dinst->getStatsFlag();
  // [sizhuo] check whether comSQ already has store to same CACHE LINE
  // we do this check before insertion of current store
  bool noOlderSt = true;
  for(ComSQ::iterator iter = comSQ.begin(); iter != comSQ.end(); iter++) {
    I(iter->second);
    if(getLineAddr(iter->second->addr) == getLineAddr(addr)) {
      I(iter->first != id);
      noOlderSt = false;
      break;
    }
  }
  // [sizhuo] insert to comSQ
  ComSQEntry *en = comSQEntryPool.out();
  en->clear();
  en->addr = addr;
  en->doStats = doStats;
  std::pair<ComSQ::iterator, bool> insertRes = comSQ.insert(std::make_pair<Time_t, ComSQEntry*>(id, en));
  I(insertRes.second);
  // [sizhuo] if comSQ doesn't have other store to same CACHE LINE, send this one to memory
  if(noOlderSt) {
    stToMemCB::scheduleAbs(stToMemPort->nextSlot(doStats), this, addr, id, doStats);
  }
}

bool WMMLSQ::retire(DInst *dinst) {
  I(dinst);
  I(!dinst->isPoisoned()); // [sizhuo] can't be poisoned
  const Instruction *const ins = dinst->getInst();
  I(ins->isLoad() || ins->isStore() || ins->isRecFence() || ins->isComFence());

  // [sizhuo] for commit fence, just check comSQ empty
  if(ins->isComFence()) {
    if(comSQ.empty()) {
      // [sizhuo] find the commit fence from LSQ
      SpecLSQ::iterator iter = specLSQ.find(dinst->getID());
      SpecLSQEntry *en = iter->second;
      I(iter == specLSQ.begin());
      I(iter != specLSQ.end());
      I(iter->first == dinst->getID());
      I(en);
      // [sizhuo] retire from spec LSQ
      specLSQ.erase(iter);
      // [sizhuo] free store entry
      I((en->pendRetireQ).empty() && (en->pendExQ).empty());
      specLSQEntryPool.in(en);
      // [sizhuo] incr free st cnt
      freeStNum++;
      I(freeStNum > 0);
      I(freeStNum <= maxStNum);
      return true; // [sizhuo] retire success
    } else {
      return false; // [sizhuo] retire fail, no need to try younger store
    }
  }

  // [sizhuo] for other inst, try to find the entry in spec LSQ
  SpecLSQ::iterator retireIter = specLSQ.find(dinst->getID());
  SpecLSQEntry *retireEn = 0;
  if(retireIter == specLSQ.end()) {
    // [sizhuo] this store is not in spec LSQ, because we have retired it early
    I(ins->isStore());
    // [sizhuo] stats: dinst is an early retired store
    I(globalClock >= dinst->getEarlyRetireTime());
    I(dinst->getEarlyRetireTime() > 0); // [sizhuo] normally store is retired when global clock > 0
    stEarlyRetireTime.sample(globalClock - dinst->getEarlyRetireTime(), dinst->getStatsFlag());
    // [sizhuo] try to find a younger store to retire only if we allow ld->st reorder
    if(orderLdSt) {
      retireEn = 0;
    } else {
      retireIter = findStToRetire();
      if(retireIter != specLSQ.end()) {
        retireEn = retireIter->second;
        I(retireEn);
        //ID(retireEn->dinst->dump("retire"));
        I(retireEn->dinst->getInst()->isStore());
        I(retireEn->state == Done);
        // [sizhuo] pend Q must be empty
        I((retireEn->pendExQ).empty());
        I((retireEn->pendRetireQ).empty());
        // [sizhuo] stats: new store is early retired due to current elder store
        retireEn->dinst->setEarlyRetireTime(globalClock);
        nStEarlyRetireByOldSt.inc(retireEn->dinst->getStatsFlag());
      } else {
        retireEn = 0;
      }
    }
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
    GI(ins->isLoad() || ins->isStore(), (retireEn->pendRetireQ).empty());
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
    // [sizhuo] XXX the retired specLSQ entry may be different from dinst
    I(retireEn->dinst->getInst()->isStore());
    sendStToComSQ(retireEn);
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
  // [sizhuo] find the line addr of this id
  ComSQ::iterator comIter = comSQ.find(id);
  I(comIter != comSQ.end());
  I(comIter->second);
  const AddrType lineAddr = getLineAddr(comIter->second->addr);

  // [sizhuo] delete all other stores to same CACHE LINE
  while(true) {
    ComSQ::iterator iter = comSQ.begin();
    for(; iter != comSQ.end(); iter++) {
      I(iter->second);
      if(getLineAddr(iter->second->addr) == lineAddr) {
        // [sizhuo] find the store to delete, break
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
}

void WMMLSQ::reset() {
  // [sizhuo] remove all reconcile & commit & store & executed load & NULL entry
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
      if(rmIns->isRecFence() || rmIns->isComFence() || rmIns->isStore() || (rmIns->isLoad() && rmEn->state == Done)) {
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

MTLSQ::SpecLSQ::iterator WMMLSQ::findStToRetire() {
  // [sizhuo] find the first store such that
  // 1. no older Reconcile or Commit
  // 2. all older loads and stores have resolved their addr & data
  // 3. all older inst has no replay reason
  // 4. all older loads to same addr are Done
  // (5. all older inst are not poisoned -- this should never happen)
  // 6. all older control inst have been executed
  //
  // Given the fact that all older loads and stores have resolved addr
  // there cannot be any violation on memory dependency among these loads/stores

  std::map<AddrType, bool> activeLdAddr; // [sizhuo] set of load addr that are still executing
  for(SpecLSQ::iterator iter = specLSQ.begin(); iter != specLSQ.end(); iter++) {
    const SpecLSQEntry *en = iter->second;
    // [sizhuo] 2. load/store with unresolved addr/data
    if(!en) {
      break;
    }
    DInst *dinst = en->dinst;
    I(dinst);
    const Instruction *ins = dinst->getInst();
    I(ins);
    // [sizhuo] 5. inst is poisoned
    // actually should not happen, because this func is not called after inst are poisoned
    if(dinst->isPoisoned()) {
      I(0);
      break;
    }
    // [sizhuo] 1. reconcile or commit encounter, no store returned
    if(ins->isRecFence() || ins->isComFence()) {
      break;
    }
    // [sizhuo] 3. load is replayed, so no store returned
    if(dinst->getReplayReason() != DInst::MaxReason) {
      I(ins->isLoad());
      break;
    }
    // [sizhuo] current mem inst won't stop us from searching for store to retire
    AddrType alignAddr = getMemOrdAlignAddr(dinst->getAddr());
    if(ins->isLoad() && en->state != Done) {
      // [sizhuo] add not finished load addr to set
      activeLdAddr.insert(std::make_pair<AddrType, bool>(alignAddr, true));
    } else if(ins->isStore()) {
      I(en->state == Done);
      // [sizhuo] 4. store addr does not match any unfinished load addr
      if(activeLdAddr.find(alignAddr) == activeLdAddr.end()) {
        // [sizhuo] 6. all older control inst have been executed
        if(gproc->allOlderCtrlDone(dinst->getID())) {
          // [sizhuo] this store can be retired, return it
          return iter;
        } else {
          // [sizhuo] no need to search for any younger store
          // because they are all subject to unresolved branch
          break;
        }
      } else {
        // [sizhuo] store addr match unfinished load addr
        // this store cannot be retured
        // we don't need to add this addr to set again
        // but we can continue the search
      }
    }
  }

  // [sizhuo] fail to find such store
  return specLSQ.end();
}

bool WMMLSQ::retireStEarly() {
  if(orderLdSt) {
    // [sizhuo] Ld->St is ordered, so cannot early retire
    return false;
  }

  SpecLSQ::iterator retireIter = findStToRetire();
  if(retireIter != specLSQ.end()) {
    SpecLSQEntry *retireEn = retireIter->second;
    I(retireEn);
    //ID(retireEn->dinst->dump("retireStEarly"));
    I(retireEn->dinst->getInst()->isStore());
    I(retireEn->state == Done);
    // [sizhuo] pend Q must be empty
    I((retireEn->pendExQ).empty());
    I((retireEn->pendRetireQ).empty());
    // [sizhuo] send the store to comSQ
    sendStToComSQ(retireEn);
    // [sizhuo] retire from spec LSQ
    specLSQ.erase(retireIter);
    // [sizhuo] recycle specLSQ entry
    specLSQEntryPool.in(retireEn);
    // [sizhuo] stats: new store is early retired due to unused retire BW
    retireEn->dinst->setEarlyRetireTime(globalClock);
    nStEarlyRetireByStall.inc(retireEn->dinst->getStatsFlag());
    return true; // [sizhuo] early retire success
  } else {
    return false; // [sizhu] early retire fail
  }
}
