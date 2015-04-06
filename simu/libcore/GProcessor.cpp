// Contributed by Jose Renau
//                Luis Ceze
//                Karin Strauss
//                Milos Prvulovic
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

#include <sys/time.h>
#include <unistd.h>
#include "GProcessor.h"
#include "Report.h"
#include "FetchEngine.h"
#include "GMemorySystem.h"
#include "MTStoreSet.h"
#include "MTLSQ.h"

GStatsCntr *GProcessor::wallClock=0;
Time_t GProcessor::lastWallClock=0;

GProcessor::GProcessor(GMemorySystem *gm, CPU_t i, size_t numFlows)
  :cpu_id(i) // [sizhuo] cpu ID
  // [sizhuo] constant params
  ,MaxFlows(numFlows)
  ,FetchWidth(SescConf->getInt("cpusimu", "fetchWidth",i))
  ,IssueWidth(SescConf->getInt("cpusimu", "issueWidth",i))
  ,RetireWidth(SescConf->getInt("cpusimu", "retireWidth",i))
  ,RealisticWidth(RetireWidth < IssueWidth ? RetireWidth : IssueWidth)
  ,InstQueueSize(SescConf->getInt("cpusimu", "instQueueSize",i))
  ,MaxROBSize(SescConf->getInt("cpusimu", "robSize",i))
  // [sizhuo] components
  ,memorySystem(gm)
  //,storeset(i)
	,mtStoreSet(0)
	,mtLSQ(0)
  ,rROB(SescConf->getInt("cpusimu", "robSize", i))
  ,ROB(MaxROBSize)
  // [sizhuo] stats
  ,rrobUsed("P(%d)_rrobUsed", i) // avg
  ,robUsed("P(%d)_robUsed", i) // avg
  ,nReplayInst("P(%d)_nReplayInst", i)
  ,nCommitted("P(%d):nCommitted", i) // Should be the same as robUsed - replayed
  ,noFetch("P(%d):noFetch", i)
  ,noFetch2("P(%d):noFetch2", i)
  ,nFreeze("P(%d):nFreeze",i)
  ,clockTicks("P(%d):clockTicks",i)
{
  active = true; // [sizhuo] set proc active

  lastReplay = 0;
  if (wallClock ==0)
    wallClock = new GStatsCntr("OS:wallClock");

  throttlingRatio = SescConf->getDouble("cpusimu", "throttlingRatio" , i);
  throttling_cntr = 0;
  
  // [sizhuo] SCOORE style
  bool scooremem = false;
  if (SescConf->checkBool("cpusimu"    , "scooreMemory" , gm->getCoreId()))
    scooremem = SescConf->getBool("cpusimu", "scooreMemory",gm->getCoreId());

  if(scooremem) {
    if((!SescConf->checkCharPtr("cpusimu", "SL0", i))&&(!SescConf->checkCharPtr("cpusimu", "VPC", i))){
      printf("ERROR: scooreMemory requested but no SL0 or VPC specified\n");
      fflush(stdout);
      exit(15);
    }
    if((SescConf->checkCharPtr("cpusimu", "SL0", i))&&(SescConf->checkCharPtr("cpusimu", "VPC", i))){
      printf("ERROR: scooreMemory requested, cannot have BOTH SL0 and VPC specified\n");
      fflush(stdout);
      exit(15);
    }
  }

  // [sizhuo] check params
  SescConf->isInt("cpusimu"    , "issueWidth" , i);
  SescConf->isLT("cpusimu"     , "issueWidth" , 1025, i);

  SescConf->isInt("cpusimu"    , "retireWidth", i);
  SescConf->isBetween("cpusimu", "retireWidth", 0   , 32700 , i);

  SescConf->isInt("cpusimu"    , "robSize"    , i);
  SescConf->isBetween("cpusimu", "robSize"    , 2   , 262144, i);

  nStall[0] = 0 ; // crash if used
  nStall[SmallWinStall]      = new GStatsCntr("P(%d)_ExeEngine:nSmallWinStall",i);
  nStall[SmallROBStall]      = new GStatsCntr("P(%d)_ExeEngine:nSmallROBStall",i);
  nStall[SmallREGStall]      = new GStatsCntr("P(%d)_ExeEngine:nSmallREGStall",i);
  nStall[OutsLoadsStall]    = new GStatsCntr("P(%d)_ExeEngine:nOutsLoadsStall",i);
  nStall[OutsStoresStall]   = new GStatsCntr("P(%d)_ExeEngine:nOutsStoresStall",i);
  nStall[OutsBranchesStall] = new GStatsCntr("P(%d)_ExeEngine:nOutsBranchesStall",i);
  nStall[ReplaysStall]      = new GStatsCntr("P(%d)_ExeEngine:nReplaysStall",i);
  nStall[SyscallStall]      = new GStatsCntr("P(%d)_ExeEngine:nSyscallStall",i);

  I(ROB.size() == 0);

  eint = 0;

  buildInstStats(nInst, "ExeEngine");

	// [sizhuo] build store set
	mtStoreSet = MTStoreSet::create(i);
	I(mtStoreSet);
	// [sizhuo] build LSQ
	mtLSQ = MTLSQ::create(this);
	I(mtLSQ);
}

GProcessor::~GProcessor() {
}


void GProcessor::buildInstStats(GStatsCntr *i[iMAX], const char *txt) {
  bzero(i, sizeof(GStatsCntr *) * iMAX);

  for(int32_t t = 0; t < iMAX; t++) {
    i[t] = new GStatsCntr("P(%d)_%s_%s:n", cpu_id, txt, Instruction::opcode2Name(static_cast<InstOpcode>(t)));
  }

  IN(forall((int32_t a=1;a<(int)iMAX;a++), i[a] != 0));
}

int32_t GProcessor::issue(PipeQueue &pipeQ) {
  int32_t i=0; // Instructions executed counter

  I(!pipeQ.instQueue.empty());

  do{ // [sizhuo] try to issue everything in instQ to ROB
    IBucket *bucket = pipeQ.instQueue.top();
    do{ // [sizhuo] try to issue all uOPs in IBucket
      I(!bucket->empty());
      if( i >= IssueWidth ) { // [sizhuo] limit by issue width
  return i;
      }

      I(!bucket->empty());

      DInst *dinst = bucket->top();
      //  GMSG(getCoreId()==1,"push to pipe %p", bucket);

      StallCause c = addInst(dinst);
      if (c != NoStall) { // [sizhuo] can't issue
  if (i < RealisticWidth)
    nStall[c]->add(RealisticWidth - i, dinst->getStatsFlag());
  return i;
      }
      i++;

      bucket->pop();

    }while(!bucket->empty());

	// this bucket is done, free it from front-end
    pipeQ.pipeLine.doneItem(bucket);
    pipeQ.instQueue.pop();
  }while(!pipeQ.instQueue.empty());

  return i;
}


void GProcessor::retire(){

}
