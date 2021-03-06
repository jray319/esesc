// Contributed by Jose Renau
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

#ifndef CLUSTER_H
#define CLUSTER_H

#include "estl.h"

#include <vector>
#include <limits.h>

#include "nanassert.h"

#include "DepWindow.h"
#include "GStats.h"
#include "Instruction.h"

class Resource;
class GMemorySystem;
class GProcessor;

// [sizhuo] base class for all types of clusters
// cluster is a superscalar functional uint (e.g. load store unit, ALU unit, FP unit, branch unit)
class Cluster {
 private:
  void buildUnit(const char *clusterName
                 ,GMemorySystem *ms
                 ,Cluster *cluster
                 ,InstOpcode type);

 protected:
  DepWindow window;

  const int32_t MaxWinSize; // [sizhuo] max issue windown size
  int32_t windowSize; // [sizhuo] current window size (free space)

  GProcessor *const gproc; // [sizhuo] processor it belongs to

  GStatsAvg  winNotUsed;
  GStatsCntr rdRegPool;
  GStatsCntr wrRegPool;

  Resource   *res[iMAX]; // [sizhuo] mapping from uOP type to function unit

	const int32_t MaxRegPool; // [sizhuo] added to recover regPool
  int32_t regPool;


 protected:
  void delEntry() {
    windowSize++;
    I(windowSize<=MaxWinSize);
  }
  void newEntry() {
    windowSize--;
    I(windowSize>=0);
  }
  
  virtual ~Cluster();
  Cluster(const char *clusterName, GProcessor *gp);

 public:
	// [sizhuo] issue stall due to small win
	GStatsCntr smallWinIssueStall;

  void select(DInst *dinst);

  // [sizhuo] things to do when dinst issued to ex, finshes ex, retires from ROB
  virtual void executing(DInst *dinst) = 0;
  virtual void executed(DInst *dinst)  = 0;
  virtual bool retire(DInst *dinst, bool replay)    = 0;

  static Cluster *create(const char *clusterName, GMemorySystem *ms, GProcessor *gproc);

  Resource *getResource(InstOpcode type) const {
    I(type < iMAX);
    return res[type];
  }

  StallCause canIssue(DInst *dinst) const; // [sizhuo] can add uOP to cluster
  void addInst(DInst *dinst); // [sizhuo] add uOP to cluster

  GProcessor *getGProcessor() const { return gproc; }

	// [sizhuo] recover state when flushing ROB
	void reset() {
		regPool = MaxRegPool;
		windowSize = MaxWinSize;
		window.reset();
		// [sizhuo] reset all resources in this cluster
		for(int32_t i = 0; i < iMAX; i++) {
			if(res[i]) {
				res[i]->reset();
			}
		}
	}

	bool isReset() {
		if(regPool != MaxRegPool || windowSize != MaxWinSize || !window.isReset()) {
			I(0);
			return false;
		}
		for(int32_t i = 0; i < iMAX; i++) {
			if(res[i]) {
				if(!res[i]->isReset()) {
					I(0);
					return false;
				}
			}
		}
		return true;
	}
 
};

class ExecutingCluster : public Cluster {
  // This is SCOORE style. The instruction is removed from the queue at dispatch time
 public:
  virtual ~ExecutingCluster() {
  }
    
  ExecutingCluster(const char *clusterName, GProcessor *gp)
    : Cluster(clusterName, gp) { }
    
  void executing(DInst *dinst);
  void executed(DInst *dinst);
  bool retire(DInst *dinst, bool replay);
};

// [sizhuo] inst is removed from issue window after execution
class ExecutedCluster : public Cluster {
 public:
  virtual ~ExecutedCluster() {
  }
    
  ExecutedCluster(const char *clusterName, GProcessor *gp)
    : Cluster(clusterName, gp) { }
    
  void executing(DInst *dinst);
  void executed(DInst *dinst);
  bool retire(DInst *dinst, bool replay);
};

// [sizhuo] inst is removed from issue window when retiring from ROB
class RetiredCluster : public Cluster {
 public:
  virtual ~RetiredCluster() {
  }
  RetiredCluster(const char *clusterName, GProcessor *gp)
    : Cluster(clusterName, gp) { }

  void executing(DInst *dinst);
  void executed(DInst *dinst);
  bool retire(DInst *dinst, bool replay);
};


#endif // CLUSTER_H
