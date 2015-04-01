#ifndef _WMMPROCESSOR_H_
#define _WMMPROCESSOR_H_

#include "nanassert.h"
#include "callback.h"
#include "GProcessor.h"
#include "Pipeline.h"
#include "FastQueue.h"
#include "FrontEnd.h"

// [sizhuo] Processor for WMM
class WMMProcessor : public GProcessor {
private:
	// [sizhuo] for checking processor deadlock
  class RetireState {
  public:
    double committed;
    Time_t r_dinst_ID;
    Time_t dinst_ID;
    DInst *r_dinst;
    DInst *dinst;    
    bool operator==(const RetireState& a) const {
      return a.committed == committed;
    };
    RetireState() {
      committed  = 0;
      r_dinst_ID = 0;
      dinst_ID   = 0;
      r_dinst    = 0;
      dinst      = 0;
    }
  };

	FrontEnd frontEnd; // [sizhuo] simplified front end pipline
	LSQNone lsq; // [sizhuo] load/store queue: currently just a dummy one

  int32_t spaceInInstQueue; // [sizhuo] remaining free space in inst queue
  DInst *RAT[LREG_MAX]; // [sizhuo] rename table

  void fetch(FlowID fid);

	// [sizhuo] check processor deadlock
	bool lockCheckEnabled;
  RetireState last_state;
  void retire_lock_check();
  StaticCallbackMember0<WMMProcessor, &WMMProcessor::retire_lock_check> retire_lock_checkCB;
protected:
  ClusterManager clusterManager;

  GStatsAvg avgFetchWidth;

	bool advance_clock(FlowID fid);
	StallCause addInst(DInst *dinst);
	void retire();
	void issueToROB();

public:
	WMMProcessor(GMemorySystem *gm, CPU_t i);
	virtual ~WMMProcessor();

	LSQ *getLSQ() { return &lsq; }
	void replay(DInst *target);
	bool isFlushing();
	bool isReplayRecovering();
	Time_t getReplayID();
};

#endif /* _WMMPROCESSOR_H_ */
