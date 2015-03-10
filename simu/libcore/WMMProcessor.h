#ifndef _WMMPROCESSOR_H_
#define _WMMPROCESSOR_H_

#include "nanassert.h"
#include "callback.h"
#include "GProcessor.h"
#include "Pipeline.h"
#include "FetchEngine.h"
#include "FastQueue.h"

// [sizhuo] Processor for WMM
class WMMProcessor : public GProcessor {
private:
  FetchEngine IFID; // [sizhuo] ifc with emulator
  PipeQueue pipeQ; // [sizhuo] front-end pipeline
	LSQNone lsq; // [sizhuo] load/store queue: currently just a dummy one

  int32_t spaceInInstQueue; // [sizhuo] remaining free space in inst queue
  DInst *RAT[LREG_MAX]; // [sizhuo] rename table

  bool busy; // [sizhuo] processor still has sth to do

  void fetch(FlowID fid);

protected:
  ClusterManager clusterManager;

  GStatsAvg avgFetchWidth;

	bool advance_clock(FlowID fid);
	StallCause addInst(DInst *dinst);
	void retire();

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
