// Contributed by Jose Renau
//                Basilio Fraguela
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

#ifndef PIPELINE_H
#define PIPELINE_H

#include <vector>
#include <set>
#include <queue>
//#include <boost/heap/priority_queue.hpp>

#include "nanassert.h"
#include "FastQueue.h"

#include "DInst.h"

/*
 * This class simulates a pipeline. Besides this simple operation, it
 * also makes possible to receive the IData out-of-order, and it
 * orders so that the execution is correct. This is very useful for
 * the I-cache because it can answer out-of-order.
 *
 * The process follows:
 *  getItem();
 *  readyItem(); // when the fetch process has finished
 *  doneItem();  // when all the instructions are executed
 */


typedef uint32_t CPU_t;
class IBucket;

class PipeIBucketLess {
 public:
  bool operator()(const IBucket *x, const IBucket *y) const;
};

// [sizhuo] this class simulates part of the front end
// it includes delay of fetch, dec, rename??
class Pipeline {
private:
  const size_t PipeLength; // [sizhuo] length of pipeline
  const size_t bucketPoolMaxSize;
  const int32_t MaxIRequests; // [sizhuo] max in flight I$ req
  int32_t nIRequests; // [sizhuo] remaining number of fetch req that can be issued (i.e. free entries in I$ controller)
  // [sizhuo] output buffer of fetched & decoded & renamed uOPs
  // renamed is not in functional part, only its timing is simulated
  FastQueue<IBucket *> buffer; 

  // [sizhuo] bucket pool for fast memory management
  typedef std::vector<IBucket *> IBucketCont;
  IBucketCont bucketPool;
  IBucketCont cleanBucketPool; // [sizhuo] what is clean??? seems never truly used

  //typedef boost::heap::priority_queue<IBucket *,boost::heap::compare<PipeIBucketLess> > ReceivedType;
  typedef std::priority_queue<IBucket *, std::vector<IBucket*>, PipeIBucketLess> ReceivedType;
  //std::priority_queue<IBucket *, std::vector<IBucket*>, PipeIBucketLess> received;
  ReceivedType received; // [sizhuo] OOO received fetch resp, ordered by program order

  // [sizhuo] the time stamp when fetch req is issued
  // this denotes program order
  Time_t maxItemCntr; // [sizhuo] this is assigned to new fetch req
  Time_t minItemCntr; // [sizhuo] fetch resp with this stamp should be deq from received queue
  
protected:
  void clearItems(); // [sizhuo] flush received queue to output buffer
public:
  // [sizhuo] s: pipeline len (delay of dec & rename)
  // fetch: fetch width in configuration
  // maxReqs: max number of in flight fetch req
  Pipeline(size_t s, size_t fetch, int32_t maxReqs);
  virtual ~Pipeline();
 
  void cleanMark();

  IBucket *newItem(); // [sizhuo] issue a new fetch req
  bool hasOutstandingItems() const; // [sizhuo] whole pipeline is not empty
  void readyItem(IBucket *b); // [sizhuo] a fetch resp comes
  void doneItem(IBucket *b); // [sizhuo] uOPs leave front-end pipeline
  IBucket *nextItem(); // [sizhuo] let buffer pop an fetched & dec & renamed IBucket (to next stage)

  size_t size() const { return buffer.size(); }
};

// [sizhuo] this denotes a bunch of uOPs fetched together
// because we are superscalar, see configuration "fetchWidth"
// it is modelled as a FIFO of uOPs in program order
class IBucket : public FastQueue<DInst *> {
private:
protected:
  const bool cleanItem; // [sizhuo] default to false, seems not truly useful

  Time_t pipeId; // [sizhuo] time stamp when fetch req is issued
  Time_t clock; // [sizhuo] global time when fetch resp returns (but not decoded or renamed)

  friend class Pipeline;
  friend class PipeIBucketLess;

  Pipeline *const pipeLine; // [sizhuo] pipeline it belongs to
  ID(bool fetched;)

  Time_t getPipelineId() const { return pipeId; }
  void setPipelineId(Time_t i) {
    pipeId=i;
  }

  void markFetched(); // [sizhuo] inform pipeline that fetch resp is ready

  Time_t getClock() const { return clock; }
  void setClock() {
    clock = globalClock;
  }
 
public:
  IBucket(size_t size, Pipeline *p, bool clean=false);
  virtual ~IBucket() { }
  // [sizhuo] class to schedule markFetched() as callback
  StaticCallbackMember0<IBucket, &IBucket::markFetched> markFetchedCB;
};


// [sizhuo] this class simulates the timing of front-end pipeline
// including: fetch, decode, rename
// although rename is after enq to instQueue, we still simulate its delay together with decode
class PipeQueue {
public:
  PipeQueue(CPU_t i); // [sizhuo] i is core id
  ~PipeQueue();

  Pipeline pipeLine; // [sizhuo] for timing simulation
  FastQueue<IBucket *> instQueue;

};


#endif // PIPELINE_H
