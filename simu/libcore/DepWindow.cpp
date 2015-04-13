// Contributed by Jose Renau
//                Basilio Fraguela
//                Smruti Sarangi
//                Luis Ceze
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

#include "DepWindow.h"

#include "DInst.h"
#include "Resource.h"
#include "SescConf.h"
#include "GProcessor.h"

DepWindow::DepWindow(GProcessor *gp, Cluster *aCluster, const char *clusterName)
  :gproc(gp)
  ,srcCluster(aCluster)
  ,Id(gp->getId())
  ,InterClusterLat(SescConf->getInt("cpusimu", "interClusterLat",gp->getId()))
  ,WakeUpDelay(SescConf->getInt(clusterName, "wakeupDelay"))
  ,SchedDelay(SescConf->getInt(clusterName, "schedDelay"))
  ,RegFileDelay(SescConf->getInt(clusterName, "regFileDelay"))
  ,wrForwardBus("P(%d)_%s_wrForwardBus",Id, clusterName)
{
  char cadena[100];
  sprintf(cadena,"P(%d)_%s_wakeUp", Id, clusterName);
  wakeUpPort = PortGeneric::create(cadena
                                 ,SescConf->getInt(clusterName, "wakeUpNumPorts")
                                 ,SescConf->getInt(clusterName, "wakeUpPortOccp"));

  SescConf->isInt(clusterName, "wakeupDelay");
  SescConf->isBetween(clusterName, "wakeupDelay", 1, 1024);

  sprintf(cadena,"P(%d)_%s_sched", Id, clusterName);
  schedPort = PortGeneric::create(cadena
                                  ,SescConf->getInt(clusterName, "SchedNumPorts")
                                  ,SescConf->getInt(clusterName, "SchedPortOccp"));

  // Constraints
  SescConf->isInt(clusterName    , "schedDelay");
  SescConf->isBetween(clusterName , "schedDelay", 0, 1024);

  SescConf->isInt("cpusimu"    , "interClusterLat",Id);
  SescConf->isBetween("cpusimu" , "interClusterLat", 0, 1024,Id);

	// [sizhuo] we require regFileDelay >= 1
	// so that inst will not be issued to func unit at the same cycle when preSelect() is called
	// This ensures we can call cluster->executing() before issuing to LSQ in WMMFUStore/WMMFULoad
	// otherwise, inst waken up in cluster->executing() might be issued to LSQ earlier
  SescConf->isInt(clusterName    , "regFileDelay");
  SescConf->isBetween(clusterName , "regFileDelay", 1, 1024);
}

DepWindow::~DepWindow() {
}

StallCause DepWindow::canIssue(DInst *dinst) const {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  return NoStall;
}

void DepWindow::addInst(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  I(dinst->getCluster() != 0); // Resource::schedule must set the resource field

  // [sizhuo] if the new instruction doesn't bear dependency, we can wake it up
  if (!dinst->hasDeps() && !dinst->hasMemDep()) {
    dinst->setWakeUpTime(wakeUpPort->nextSlot(dinst->getStatsFlag()) + WakeUpDelay);
    preSelect(dinst);
  }
}

// Look for dependent instructions on the same cluster (do not wakeup,
// just get the time)
// [sizhuo] wake up dinst which has resolved data dependency
void DepWindow::wakeUpDeps(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  I(!dinst->hasDeps());
	I(!dinst->hasMemDep());

  // [sizhuo] why we need to just increase wake up time of a inst??
  // since wake up port is configured to have 0 occupation time
  // this extra use of wake up port doesn't matter...

  // Even if it does not wakeup instructions the port is used
  Time_t wakeUpTime= wakeUpPort->nextSlot(dinst->getStatsFlag());
  //dinst->dump("Clearing:");

  if (!dinst->hasPending() && !dinst->hasMemPending())
    return;

  // NEVER HERE FOR in-order cores

  wakeUpTime += WakeUpDelay;

  I(dinst->getCluster());
  I(srcCluster == dinst->getCluster());

  // [sizhuo] for all other inst depending on me, try to increase their minimum wake up time
  if(dinst->hasPending()) {
		for(const DInstNext *it = dinst->getFirst();
				 it ;
				 it = it->getNext() ) {
			DInst *dstReady = it->getDInst();

			const Cluster *dstCluster = dstReady->getCluster();
			I(dstCluster); // all the instructions should have a resource after rename stage

			// [sizhuo] only increase wakeup time of inst in same cluster
			if (dstCluster == srcCluster && dstReady->getWakeUpTime() < wakeUpTime)
				dstReady->setWakeUpTime(wakeUpTime);
		}
	}
	// [sizhuo] increase wake up time for memory dependency
	if(dinst->hasMemPending()) {
		for(const DInstNext *it = dinst->getMemFirst();
				 it ;
				 it = it->getNext() ) {
			DInst *dstReady = it->getDInst();

			const Cluster *dstCluster = dstReady->getCluster();
			I(dstCluster); // all the instructions should have a resource after rename stage

			// [sizhuo] only increase wakeup time of inst in same cluster
			if (dstCluster == srcCluster && dstReady->getWakeUpTime() < wakeUpTime)
				dstReady->setWakeUpTime(wakeUpTime);
		}
	}
}

// [sizhuo] schedule a callback to select the dinst for execution (NOT callback to execute dinst)
void DepWindow::preSelect(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  // At the end of the wakeUp, we can start to read the register file
  I(dinst->getWakeUpTime());
  I(!dinst->hasDeps());
	I(!dinst->hasMemDep());

  Time_t wakeUpTime = dinst->getWakeUpTime() + RegFileDelay;

  IS(dinst->setWakeUpTime(0));
  dinst->markIssued();
  I(dinst->getCluster());
  //dinst->clearRATEntry(); // [sizhuo] we should not touch rename table

  // [sizhuo] schedule the resource of dinst to call select()
  // actually it finally let this DepWindow to call select()
	// require wakeUpTime strictly larger than current time
	I(wakeUpTime > globalClock);
  Resource::selectCB::scheduleAbs(wakeUpTime, dinst->getClusterResource(), dinst);
}

// [sizhuo] select dinst for execution
void DepWindow::select(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  I(!dinst->getWakeUpTime());

  Time_t schedTime = schedPort->nextSlot(dinst->getStatsFlag()) + SchedDelay;

  I(srcCluster == dinst->getCluster());
  //dinst->executingCB.scheduleAbs(schedTime);
  // [sizhuo] schedule the cluster to execute dinst
  Resource::executingCB::scheduleAbs(schedTime, dinst->getClusterResource(), dinst);
}

// Called when dinst finished execution. Look for dependent to wakeUp
void DepWindow::executed(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  //  MSG("execute [0x%x] @%lld",dinst, globalClock);

  I(!dinst->hasDeps());
	I(!dinst->hasMemDep());

  //dinst->dump("Clearing2:");

  if (!dinst->hasPending() && !dinst->hasMemPending())
    return;

  // NEVER HERE FOR in-order cores

  I(dinst->getCluster());
  I(srcCluster == dinst->getCluster());

  // Only until reaches last. The instructions that are from another processor
  // should be added again to the dependence chain so that MemRequest::ack can
  // awake them (other processor instructions)

  const DInst *stopAtDst = 0;

  I(dinst->isIssued());
  while (dinst->hasPending()) {

    if (stopAtDst == dinst->getFirstPending()) // [sizhuo] list empty, stop
      break;
		// [sizhuo] remove dependency to dstReady
    DInst *dstReady = dinst->getNextPending();
    I(dstReady);

#if 0
    if (!dstReady->isIssued()) {
      I(dinst->getInst()->isStore());

      I(!dstReady->hasDeps());
      continue;
    }
#endif
    I(!dstReady->isExecuted());

		// [sizhuo] wake up dstReady if it has no dependency now
    if (!dstReady->hasDeps() && !dstReady->hasMemDep()) {
      // Check dstRes because dstReady may not be issued
      I(dstReady->getCluster());
      const Cluster *dstCluster = dstReady->getCluster();
      I(dstCluster);

			// [sizhuo] this function is called by a callback, and wake up port is unlimited
			// XXX: the selectCB scheduled in preSelect will be called in the same cycle...
      Time_t when = wakeUpPort->nextSlot(dinst->getStatsFlag());
      if (dstCluster != srcCluster) {
        wrForwardBus.inc(dinst->getStatsFlag());
        when += InterClusterLat;
      }

      dstReady->setWakeUpTime(when);

      preSelect(dstReady);
    }
  }
	// [sizhuo] mem dep should have been resolved
	I(!dinst->hasMemPending());
}

void DepWindow::resolveMemDep(DInst *dinst) {
	// [sizhuo] should not be poisoned inst
	I(!dinst->isPoisoned());

  I(!dinst->hasDeps());
	I(!dinst->hasMemDep());

  if (!dinst->hasMemPending()) {
    return;
	}

  I(dinst->getCluster());
  I(srcCluster == dinst->getCluster());

  const DInst *stopAtDst = 0;
  I(dinst->isIssued());
  while(dinst->hasMemPending()) {
    if (stopAtDst == dinst->getMemFirstPending()) {
			// [sizhuo] list empty, stop
      break;
		}
		// [sizhuo] remove dependency to dstReady
    DInst *dstReady = dinst->getNextMemPending();
    I(dstReady);
    I(!dstReady->isExecuted());
		// [sizhuo] wake up dstReady if it has no dependency now
    if (!dstReady->hasDeps() && !dstReady->hasMemDep()) {
      I(dstReady->getCluster());
      const Cluster *dstCluster = dstReady->getCluster();
      I(dstCluster);
			// [sizhuo] this function is called by a callback, and wake up port is unlimited
			// XXX: the selectCB scheduled in preSelect will be called in the same cycle...
      Time_t when = wakeUpPort->nextSlot(dinst->getStatsFlag());
      if (dstCluster != srcCluster) {
        wrForwardBus.inc(dinst->getStatsFlag());
        when += InterClusterLat;
      }
			// [sizhuo] wake up dstReady
      dstReady->setWakeUpTime(when);
      preSelect(dstReady);
    }
  }
}

