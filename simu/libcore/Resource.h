// Contributed by Jose Renau
//                Basilio Fraguela
//                Smruti Sarangi
//                Karin Strauss
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

#ifndef RESOURCE_H
#define RESOURCE_H

#include "GStats.h"

#include "callback.h"
#include "BloomFilter.h"
#include "FastQueue.h"

#include "nanassert.h"
#include "StoreSet.h"


class PortGeneric;
class DInst;
class MemObj;
class Cluster;

enum StallCause {
  NoStall=0,
  SmallWinStall,
  SmallROBStall,
  SmallREGStall,
  OutsLoadsStall,
  OutsStoresStall,
  OutsBranchesStall,
  ReplaysStall,
  SyscallStall,
  MaxStall,
  Suspend 
};

class GProcessor;
class LSQ;

// [sizhuo] base class for all kinds of architecture components
class Resource {
protected:
  Cluster     *const cluster; // [sizhuo] which cluster this component belongs to
  PortGeneric *const gen; // [sizhuo] the port used to model contention???
  GProcessor  *const gproc; // [sizhuo] which processor this component belongs to

  const TimeDelta_t lat;

  Time_t       usedTime;

  Resource(Cluster *cls, PortGeneric *gen, TimeDelta_t l);


public:
  virtual ~Resource();

  const Cluster *getCluster() const { return cluster; }
  Cluster *getCluster() { return cluster; }

  // Sequence:
  //
  // 1st) A canIssue check is done with "canIssue". This checks that the cluster
  // can accept another request (cluster window size), and that additional
  // structures (like the LD/ST queue entry) also have enough resources.
  //
  // 2nd) The timing to calculate when the inputs are ready is done at
  // executing.
  //
  // 3rd) executed is called the instructions has been executed. It may be
  // called through DInst::doAtExecuted
  //
  // 4th) When the instruction is retired from the ROB retire is called

  // [sizhuo] whether dinst can be issued into this function unit
	// XXX: this function has side effect, if returns NoStall, must call addInst in cluster
  virtual StallCause canIssue(DInst  *dinst) =    0;
  // [sizhuo] start to execute dinst
  virtual void       executing(DInst *dinst) =    0;
  // [sizhuo] dinst finishes execution
  virtual void       executed(DInst  *dinst) =    0;
  // [sizhuo] dinst is ready to retire, i.e. moved from ROB to rROB in GProcessor
  virtual bool       preretire(DInst  *dinst,bool flushing) = 0;
  // [sizhuo] dinst is truly retired, i.e. removed from rROB in GProcessor
  virtual bool       retire(DInst    *dinst, bool flushing) = 0;
  virtual void       performed(DInst *dinst) =    0;

  void select(DInst *dinst);

  typedef CallbackMember1<Resource, DInst *, &Resource::select>    selectCB;
  typedef CallbackMember1<Resource, DInst *, &Resource::executing> executingCB;
  typedef CallbackMember1<Resource, DInst *, &Resource::executed>  executedCB;
  typedef CallbackMember1<Resource, DInst *, &Resource::performed> performedCB;

  Time_t getUsedTime() const { return usedTime; }
  void setUsedTime() { usedTime = globalClock;  }

	// [sizhuo] recover state when flushing ROB (optional)
	virtual void flush() {}
};

class GMemorySystem;

class MemReplay : public Resource {
protected:
  const uint32_t lfSize;

  StoreSet      *storeset;
  void replayManage(DInst *dinst);
  struct FailType {
    SSID_t ssid;
    Time_t id;
    AddrType pc;
    AddrType addr;
    AddrType data;
    InstOpcode op;
  };
  FailType *lf;
public:
  MemReplay(Cluster *cls, PortGeneric *gen, StoreSet *ss, TimeDelta_t l);

};

// [sizhuo] base type for load store units
class MemResource : public MemReplay {
private:
protected:
  MemObj        *DL1; // [sizhuo] the memory component this object is accessing???
  GMemorySystem *memorySystem;
  LSQ           *lsq;

  GStatsCntr    stldViolations;

  MemResource(Cluster *cls, PortGeneric *aGen, LSQ *lsq, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad);
public:
};

// [sizhuo] base class for load store units without memory speculation
// memory speculation seems to mean "address speculation"
// because construct function doesn't take store set as input
class MemResource_noMemSpec : public Resource {
private:
protected:
  MemObj        *DL1;
  GMemorySystem *memorySystem;
  MemResource_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad);
public:
};

class SCOOREMem : public MemReplay {
private:
protected:
  MemObj        *DL1;
  MemObj        *vpc;
  GMemorySystem *memorySystem;
  //GStatsCntr    ScooreUpperHierReplays;
  //std::map<AddrType,DInst> pendLoadBuff;

  const bool enableDcache;

  TimeDelta_t    vpcDelay;
  TimeDelta_t    DL1Delay;
  
  SCOOREMem(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad);

public:
};

class FUSCOORELoad : public SCOOREMem {
private:
protected:
  void retryvpc(DInst *dinst);
  typedef CallbackMember1<FUSCOORELoad, DInst *, &FUSCOORELoad::retryvpc> retryvpcCB;
public:
  FUSCOORELoad(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
  int32_t    maxLoads;
  int32_t    free_entries;

};

class FUSCOOREStore : public SCOOREMem {
private:
protected:
  void retryvpc(DInst *dinst);
  typedef CallbackMember1<FUSCOOREStore, DInst *, &FUSCOOREStore::retryvpc> retryvpcCB;

public:
  FUSCOOREStore(Cluster *cls, PortGeneric *aGen, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
  int32_t    maxStores;
  int32_t    free_entries;


};

// [sizhuo] load unit
class FULoad : public MemResource {
private:
  const TimeDelta_t LSDelay;

  int32_t freeEntries;
  bool enableDcache;

protected:
  void cacheDispatched(DInst *dinst);
  typedef CallbackMember1<FULoad, DInst *, &FULoad::cacheDispatched> cacheDispatchedCB;

public:
  FULoad(Cluster *cls, PortGeneric *aGen, LSQ *lsq, StoreSet *ss, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
};

// [sizhuo] store unit
class FUStore : public MemResource {
private:

  int32_t freeEntries;
  bool    enableDcache;

public:
  FUStore(Cluster *cls, PortGeneric *aGen, LSQ *lsq, StoreSet *ss, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
};

class FUGeneric : public Resource {
private:

protected:
public:
  FUGeneric(Cluster *cls, PortGeneric *aGen, TimeDelta_t l);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
	// [sizhuo] don't need recovery from ROB flush
	virtual void flush() {}
};

class FUFuze : public Resource {
private:

protected:
public:
  FUFuze(Cluster *cls, PortGeneric *aGen, TimeDelta_t l);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
};

class FUBranch : public Resource {
private:
	const int32_t maxBranches; // [sizhuo] used to recover freeBranches in ROB flush
  int32_t freeBranches;

protected:
public:
  FUBranch(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, int32_t mb);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
	// [sizhuo] when ROB flush, release all branch entries
	virtual void flush() { freeBranches = maxBranches; }
};

class FURALU : public Resource {
private:
  GStatsCntr memoryBarrier;
  Time_t blockUntil;
  bool scooreMemory;

protected:
public:
  FURALU(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, bool scooreMemory, int32_t id);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst *dinst, bool flushing);
  bool       retire(DInst    *dinst, bool flushing);
  void       performed(DInst *dinst);
};

class FULoad_noMemSpec : public MemResource_noMemSpec {
private:
  const TimeDelta_t LSDelay;

  int32_t freeEntries;
  bool enableDcache;

protected:
  void cacheDispatched(DInst *dinst);
  typedef CallbackMember1<FULoad_noMemSpec, DInst *, &FULoad_noMemSpec::cacheDispatched> cacheDispatchedCB;

public:
  FULoad_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t lsdelay, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
};

class FUStore_noMemSpec : public MemResource_noMemSpec {
private:

  int32_t freeEntries;
  bool    enableDcache;

public:
  FUStore_noMemSpec(Cluster *cls, PortGeneric *aGen, TimeDelta_t l, GMemorySystem *ms, int32_t size, int32_t id, const char *cad);

  StallCause canIssue(DInst  *dinst);
  void       executing(DInst *dinst);
  void       executed(DInst  *dinst);
  bool       preretire(DInst  *dinst, bool flushing);
  bool       retire(DInst    *dinst,  bool flushing);
  void       performed(DInst *dinst);
};


#endif   // RESOURCE_H


