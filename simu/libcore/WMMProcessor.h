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
	// we use ID because ID won't be recycled
	bool flushing; // [sizhuo] flush the inst in ROB

#ifdef DEBUG
	// [sizhuo] debug for whether killed inst are re-fetched
	bool lastReplayValid;
	AddrType lastReplayPC;
	Instruction lastReplayInst;
	// [sizhuo] min value of next commit inst ID
	Time_t minComID;
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

  GStatsAvg avgFetchWidth;
	GStatsCntr nKilled;
	GStatsCntr nExcep;

  void fetch(FlowID fid);
	StallCause addInst(DInst *dinst);
	void issueToROB();
	void retireFromROB(FlowID fid);
	bool advance_clock(FlowID fid);

public:
	WMMProcessor(GMemorySystem *gm, CPU_t i);
	virtual ~WMMProcessor();

	LSQ *getLSQ() { return 0; } // [sizhuo] LSQ is obsolete, use GProcessor::getMTLSQ
	void replay(DInst *target);
	bool isFlushing() { return flushing; }
	bool isReplayRecovering() { return replayRecover; }
	Time_t getReplayID() { return replayID; }
};

#endif /* _WMMPROCESSOR_H_ */
