#ifndef MT_LSQ_H
#define MT_LSQ_H

#include "estl.h"
#include "DInst.h"
#include "Resource.h"
#include "GStats.h"
#include "MemRequest.h"
#include "SescConf.h"
#include "DInst.h"
#include <queue>
#include <map>

class GProcessor;
class MemObj;
class MTStoreSet;

// [sizhuo] multi-thread/core LSQ base class
// XXX: data forwarding must have same byte address
class MTLSQ {
protected:
  // [sizhuo] memory ordering granularity -- WORD
  // we detect st-ld bypass, memory dependency, consistency ordering
  // all based on this granularity
  // FIXME: this is inaccurate
  static const uint32_t memOrdAlignShift;

  static inline AddrType getMemOrdAlignAddr(AddrType byteAddr) {
    return byteAddr >> memOrdAlignShift;
  }

  GProcessor *gproc; // [sizhuo] pointer to processor
  MTStoreSet *mtStoreSet; // [sizhuo] pointer to store set
  MemObj *DL1; // [sizhuo] pointer to D$

  // [sizhuo] L1 cache line granularity
  const uint32_t log2LineSize; // [sizhuo] log2(cache line size)
  AddrType inline getLineAddr(AddrType byteAddr) {
    return byteAddr >> log2LineSize;
  }

  // [sizhuo] max ld/st number
  const int32_t maxLdNum;
  const int32_t maxStNum;
  // [sizhuo] fre ld/st entries
  int32_t freeLdNum;
  int32_t freeStNum;
  // [sizhuo] forward delay
  const TimeDelta_t ldldForwardDelay;
  const TimeDelta_t stldForwardDelay;

  // [sizhuo] prefetch type
  enum StPrefetch {
    None, // no prefetch
    All, // prefetch for all stores
    Select // prefetch for stores not in SQ
  };
  static StPrefetch getStPrefetchType(const char *type) {
    if(!strcasecmp(type, "none")) {
      return None;
    } else if(!strcasecmp(type, "all")) {
      return All;
    } else if(!strcasecmp(type, "select")) {
      return Select;
    } else {
      MSG("ERROR: unkown prefetch type %s", type);
      SescConf->notCorrect();
      return None;
    }
  }
  const StPrefetch prefetch;

  // [sizhuo] pending event Q type
  typedef std::queue<CallbackBase*> PendQ;

  // [sizhuo] speculative LSQ hold load/store still in ROB (speculative, can be killed)
  // load can have all 3 states
  // store only has Done state
  typedef enum {
    Wait, // [sizhuo] info (e.g. addr) filled in, but haven't issued to memory
    Exe, // [sizhuo] executing in memory OR bypassing data
    Done // [sizhuo] finished execution
  } SpecLSQState;

  class SpecLSQEntry {
  public:
    DInst *dinst;
    SpecLSQState state;
    Time_t ldSrcID; // [sizhuo] ID of the store supplying data to the load, 0 -- load from memory
    bool needReEx; // [sizhuo] memory dependency violation detected while executing in memory
    PendQ pendRetireQ; // [sizhuo] events stalled unitl this RECONCILE fence retires
    PendQ pendExQ; // [sizhuo] events stalled until this LOAD finishes execution

    // [sizhuo] for SC/TSO to kill/re-ex loads
    bool forwarding; // [sizhuo] this load is being forwarded
    bool stale; // [sizhuo] the load result is stale, should not forward it to other loads

    // [sizhuo] load verification state
    enum LdVerifyState {
      Good,
      Need,
      Verifying
    };
    LdVerifyState verify;

    SpecLSQEntry() : dinst(0), state(Wait), ldSrcID(DInst::invalidID), needReEx(false), forwarding(false), stale(false), verify(Good) {}
    void clear() {
      dinst = 0;
      state = Wait;
      ldSrcID = DInst::invalidID;
      needReEx = false;
      forwarding = false;
      stale = false;
      verify = Good;
      I(pendRetireQ.empty());
      I(pendExQ.empty());
    }
  };

  // [sizhuo] pool of SpecLSQ entries
  pool<SpecLSQEntry> specLSQEntryPool;

  typedef std::map<Time_t, SpecLSQEntry*> SpecLSQ;
  SpecLSQ specLSQ;

  // [sizhuo] commited SQ holds stores retired from ROB (non-speculative)
  class ComSQEntry {
  public:
    AddrType addr;
    bool doStats;

    ComSQEntry() : addr(0), doStats(true) {}
    void clear() {
      addr = 0;
      doStats = true;
    }
  };

  // [sizhuo] pool of comSQ entries
  pool<ComSQEntry> comSQEntryPool; 

  typedef std::map<Time_t, ComSQEntry*> ComSQ;
  ComSQ comSQ;

  // [sizhuo] fully pipelined contention port for executing Load
  PortGeneric *ldExPort;

  // [sizhuo] execute a load
  virtual void ldExecute(DInst *dinst) = 0;
  typedef CallbackMember1<MTLSQ, DInst*, &MTLSQ::ldExecute> ldExecuteCB;

  // [sizhuo] due to contention on load ex port, we need to function to schedule load ex
  void scheduleLdEx(DInst *dinst);
  typedef CallbackMember1<MTLSQ, DInst*, &MTLSQ::scheduleLdEx> scheduleLdExCB;

  // [sizhuo] when a load finishes execution
  virtual void ldDone(DInst *dinst);
  typedef CallbackMember1<MTLSQ, DInst*, &MTLSQ::ldDone> ldDoneCB;

  // [sizhuo] fully pipelined contention port for issuing store to memory
  PortGeneric *stToMemPort;

  // [sizhuo] issue store to memory
  void stToMem(AddrType addr, Time_t id, bool doStats) {
    MemRequest::sendReqWrite(DL1, doStats, addr, stCommitedCB::create(this, id));
  }
  typedef CallbackMember3<MTLSQ, AddrType, Time_t, bool, &MTLSQ::stToMem> stToMemCB;

  // [sizhuo] when a store commited to memory, we retire entry from comSQ
  // and send next oldest store to same addr to memory
  virtual void stCommited(Time_t id) = 0;
  typedef CallbackMember1<MTLSQ, Time_t, &MTLSQ::stCommited> stCommitedCB;

  // [sizhuo] XXX: we don't need contention port for ldDone & stCommited
  // they are limited by when we send ack in ACache
  // for load, we fill in data array or directly read data array
  // for store, we write data array
  // so they will contend for data array port in ACache
  
  // [sizhuo] Stats counters
  GStatsCntr nLdStallByLd;
  GStatsCntr nLdStallByRec;
  GStatsCntr nLdKillByLd;
  GStatsCntr nLdKillBySt;
  GStatsCntr nLdKillByInv;
  GStatsCntr nLdKillByRep;
  GStatsCntr nLdReExByLd;
  GStatsCntr nLdReExBySt;
  GStatsCntr nLdReExByInv;
  GStatsCntr nLdReExByRep;
  GStatsCntr nStLdForward;
  GStatsCntr nLdLdForward;
  GStatsCntr nVerifyLdByInv;
  GStatsCntr nVerifyLdByRep;

  GStatsAvg  stEarlyRetireTime; // [sizhuo] average of how much time that a store is retired early
  GStatsCntr nStEarlyRetireByOldSt; // [sizhuo] old store already retired, so do a new one
  GStatsCntr nStEarlyRetireByStall; // [sizhuo] ROB retire stall

  GStatsCntr nUnalignLd;
  GStatsCntr nUnalignSt;

  // [sizhuo] stats variables
  int32_t exLdNum; // [sizhuo] load in execution
  int32_t doneLdNum; // [sizhuo] executed load
  void inline incrExLdNum() {
    exLdNum++; 
    I(exLdNum + doneLdNum <= maxLdNum);
  }

  // [sizhuo] helper to remove poisoned entry & reset
  void removePoisonedEntry(SpecLSQ::iterator rmIter);
  void removePoisonedInst(DInst *dinst);

  // [sizhuo] search stores to same cache line
  bool matchStLine(AddrType byteAddr);

public:
  MTLSQ(GProcessor *gproc_);
  virtual ~MTLSQ() {}

  // [sizhuo] add new entry when inst is enq to ROB
  virtual StallCause addEntry(DInst *dinst) = 0;
  // [sizhuo] issue to LSQ: fill in info, check memory dependency violation
  // then LSQ will try to execute this inst
  // After execution, will call executed() of the resource
  // XXX: contention for issue is modelled in Resource::executing
  virtual void issue(DInst *dinst) = 0;
  typedef CallbackMember1<MTLSQ, DInst*, &MTLSQ::issue> issueCB;
  // [sizhuo] retire entry from speculative LSQ
  virtual bool retire(DInst *dinst) = 0;
  // [sizhuo] cache eviction may kill loads
  // lineAddr is the address of cache line
  // isReplace indicates whether cache evict is due to replacement
  virtual void cacheEvict(AddrType lineAddr, bool isReplace) = 0;
  typedef CallbackMember2<MTLSQ, AddrType, bool, &MTLSQ::cacheEvict> cacheEvictCB;
  // [sizhuo] for reset when execption happens
  virtual void reset();
  virtual bool isReset();
  // [sizhuo] retire store early
  virtual bool retireStEarly() { return false; }

  // [sizhuo] return stats to processor
  int32_t getLdQUsage() { return maxLdNum - freeLdNum; }
  int32_t getExLdNum() { return exLdNum; }
  int32_t getDoneLdNum() { return doneLdNum; }
  int32_t getStQUsage() { return maxStNum - freeStNum; }
  int32_t getComSQUsage() { return comSQ.size(); }

  // [sizhuo] do prefetch
  void doPrefetch(AddrType byteAddr, bool doStats);

  // [sizhuo] creator function
  static MTLSQ *create(GProcessor *gproc_);
};

// [sizhuo] LSQ for WMM
// 
// reconcile fence is added to LSQ when added to cluster (Resource::canIssue -- addEntry)
//
// commit fence is NOT added to LSQ
// 
// For load/store, a NULL ptr is added to specLSQ and free entry num is reduced
// when it is added to cluster (Resource::canIssue -- addEntry)
// Real entry of load/store are added when data dependency is resolved (Resource::executing -- issueCB)
// 
// for LL & SC & commit fence, we use processor retire stage to prompt its execution
// DON'T let them depend on commited store queue, it's hard to flush
//
// Commited store queue is coalesced, req to same cache line will be sent to D$ together
//
// FIXME: currently there is no LL/SC generated by emulator
// so we don't handle them
class WMMLSQ : public MTLSQ {
private:
  const bool orderLdLd; // [sizhuo] order loads on same addr
  const bool orderLdSt; // [sizhuo] order Ld --po--> St

  // [sizhuo] helper func to send st to comSQ
  void sendStToComSQ(SpecLSQEntry *retireEn);

protected:
  virtual void ldExecute(DInst *dinst);
  virtual void stCommited(Time_t id);

  SpecLSQ::iterator findStToRetire();

public:
  WMMLSQ(GProcessor *gproc_, bool ldldOrder, bool ldstOrder);
  virtual ~WMMLSQ() {}

  virtual StallCause addEntry(DInst *dinst);
  virtual void issue(DInst *dinst);
  virtual bool retire(DInst *dinst); // [sizhuo] this also tries to send a store to comSQ early
  virtual void cacheEvict(AddrType lineAddr, bool isReplace) {};
  // [sizhuo] since we may have NULL ptr in specLSQ, reset needs to clear these NULL entries
  virtual void reset();
  // [sizhuo] retire store early
  virtual bool retireStEarly();
};

// [sizhuo] LSQ for SC & TSO
// reconcile fence is treated as NOP, never inserted into LSQ
// commit fence only has effect in TSO, but it is never inserted into LSQ in both SC & TSO
// load is only killed by store or cache invalidation, not load
class SCTSOLSQ : public MTLSQ {
private:
  const bool isSC; // [sizhuo] true: SC, false: TSO
  const bool ldWait; // [sizhuo] load will be stalled if there is older load to same addr not finished
  const bool verifyLd; // [sizhuo] do load verification (just assume verify success)
  Time_t lastComStID; // [sizhuo] inst ID of last store commited to memory

protected:
  virtual void ldExecute(DInst *dinst);
  virtual void stCommited(Time_t id);

  // [sizhuo] after verification load returns
  // we always assume it success
  void ldVerified(SpecLSQEntry *en) {
    I(en);
    I(en->verify == SpecLSQEntry::Verifying);
    en->verify = SpecLSQEntry::Good;
  }
  typedef CallbackMember1<SCTSOLSQ, SpecLSQEntry*, &SCTSOLSQ::ldVerified> ldVerifiedCB;

  // [sizhuo] issue verification load to memory
  void doLdVerify(SpecLSQEntry *en) {
    I(en);
    I(en->verify == SpecLSQEntry::Verifying);
    DInst *dinst = en->dinst;
    const bool doStats = dinst->getStatsFlag();
    I(dinst->getInst()->isLoad());
    // [sizhuo] stats
    if(dinst->getReplayReason() == DInst::CacheInv) {
      nVerifyLdByInv.inc(doStats);
    } else {
      I(dinst->getReplayReason() == DInst::CacheRep);
      nVerifyLdByRep.inc(doStats);
    }
    // [sizhuo] issue verification load to memory
    MemRequest::sendReqRead(DL1, doStats, dinst->getAddr(), ldVerifiedCB::create(this, en));
  }
  typedef CallbackMember1<SCTSOLSQ, SpecLSQEntry*, &SCTSOLSQ::doLdVerify> doLdVerifyCB;

public:
  SCTSOLSQ(GProcessor *gproc_, bool sc, bool wait, bool verify);
  virtual ~SCTSOLSQ() {}

  virtual StallCause addEntry(DInst *dinst);
  virtual void issue(DInst *dinst);
  virtual bool retire(DInst *dinst);
  virtual void cacheEvict(AddrType lineAddr, bool isReplace);
};

#endif
