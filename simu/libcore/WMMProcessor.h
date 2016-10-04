#ifndef _WMMPROCESSOR_H_
#define _WMMPROCESSOR_H_

#include "nanassert.h"
#include "callback.h"
#include "GProcessor.h"
#include "Pipeline.h"
#include "FastQueue.h"
#include "FrontEnd.h"
#include <deque>

// [sizhuo] Processor for WMM
class WMMProcessor : public GProcessor {
private:
  // [sizhuo] simulation stage
  enum SimStage {Forward, Sim, Done};
  static SimStage simStage;
  static Time_t simBeginTime;
  static GStatsCntr *simTime;

  // [sizhuo] for checking processor deadlock
  class RetireState {
  public:
    double committed;
    double killed;
    Time_t dinst_ID;
    DInst *dinst;    
    bool operator==(const RetireState& a) const {
      return a.committed == committed && a.killed == killed;
    };
    RetireState() {
      committed  = 0;
      killed     = 0;
      dinst_ID   = 0;
      dinst      = 0;
    }
  };

  // [sizhuo] use double-queue as ROB to simulator ROB flush
  // XXX: we no longer use ROB & rROB from GProcessor class
  std::deque<DInst*> rob;
  FrontEnd frontEnd; // [sizhuo] simplified front end pipline
  DInst *RAT[LREG_MAX]; // [sizhuo] rename table

  // [sizhuo] ROB flush state
  // replay recover procedure:
  // 1. function unit (e.g. WMMFULoad) detects violation and call gproc->replay(dinst)
  // 2. ID of dinst is recorded in replayID & replayRecover is set
  // 3. when replayRecover is set, stop fetching, stop issuing to ROB
  // 4. when inst with replayID reaches ROB head, set flushing
  // 5. when flushing is set: 
  //    (1) poison all inst in ROB
  //    (2) reset each function unit state & rename table & other components
  //    (3) drain ROB from tail, drain front-end
  // 6. when ROB is empty & front-end is empty, finish replay recover
  bool replayRecover; // [sizhuo] exception detected
  Time_t replayID; // [sizhuo] ID of inst to be killed & restarted
  DInst::ReplayReason replayReason; // [sizhuo] reason for replay
  // we use ID because ID won't be recycled
  bool flushing; // [sizhuo] flush the inst in ROB

#ifdef DEBUG
  // [sizhuo] debug for whether killed inst are re-fetched
  bool lastReplayValid;
  AddrType lastReplayPC;
  Instruction lastReplayInst;
  // [sizhuo] last commit inst ID
  Time_t lastComID;
  // [sizhuo] debug function for flush: randomly generate exception
  bool startExcep;
  const TimeDelta_t excepDelay;
  void genExcep();
  StaticCallbackMember0<WMMProcessor, &WMMProcessor::genExcep> genExcepCB;
#endif

  // [sizhuo] check processor deadlock
  bool lockCheckEnabled;
  RetireState last_state;
  void retire_lock_check();
  StaticCallbackMember0<WMMProcessor, &WMMProcessor::retire_lock_check> retire_lock_checkCB;

  // [sizhuo] helper functions for replay & flush
  void reset();
  bool isReset();
  void doFlush();


protected:
  ClusterManager clusterManager;

  void fetch(FlowID fid);
  StallCause addInst(DInst *dinst);
  void issueToROB();
  void retireFromROB(FlowID fid);
  bool advance_clock(FlowID fid);

  // [sizhuo] number of active cycles
  GStatsCntr nActiveCyc;

  // [sizhuo] number of replay (mem dependency/consistency violation)
  GStatsCntr *nExcep[DInst::MaxReason];
  // [sizhuo] number of inst killed due to exception
  GStatsCntr *nKilled[DInst::MaxReason];

  // [sizhuo] stats for ROB & LSQ usage
  GStatsHist robUsage;
  GStatsHist ldQUsage;
  GStatsHist exLdNum;
  GStatsHist doneLdNum;
  GStatsHist stQUsage;
  GStatsHist comSQUsage;

  // [sizhuo] stats for ROB retire port
  GStatsCntr retireStallByEmpty; // [sizhuo] empty ROB
  GStatsCntr *retireStallByEx[iMAX]; // [sizhuo] inst not finished execution
  GStatsCntr retireStallByComSQ; // [sizhuo] commit fence in WMM/TSO OR load in SC
  GStatsCntr *retireStallByFlush[DInst::MaxReason]; // [sizhuo] ROB is flushing poisoned inst
  GStatsCntr *retireStallByVerify[DInst::MaxReason]; // [sizhuo] load verification

  // [sizhuo] stats for ROB issue port
  GStatsCntr *issueStall[MaxStall];
  GStatsCntr *issueStallByReplay[DInst::MaxReason];

public:
  WMMProcessor(GMemorySystem *gm, CPU_t i);
  virtual ~WMMProcessor();

  LSQ *getLSQ() { return 0; } // [sizhuo] LSQ is obsolete, use GProcessor::getMTLSQ
  void replay(DInst *target);
  bool isFlushing() { return flushing; }
  bool isReplayRecovering() { return replayRecover; }
  Time_t getReplayID() { return replayID; }

  virtual Time_t getROBHeadID() {
    GI(!rob.empty(), rob.front());
    return rob.empty() ? DInst::invalidID : (rob.front())->getID();
  }

  virtual bool allOlderCtrlDone(Time_t id);
};

#endif /* _WMMPROCESSOR_H_ */
