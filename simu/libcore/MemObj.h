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

#ifndef MEMOBJ_H
#define MEMOBJ_H

#include "DInst.h"
#include "callback.h"

#include "nanassert.h"

#include "Resource.h"
#include "MRouter.h"

class MemRequest;
class MTLSQ;

class MemObj {
private:
protected:
  friend class MRouter;

  MRouter *router; // [sizhuo] routing table for where msg to go from this MemObj
  const char *section;
  const char *name;
  const uint16_t id;
  static uint16_t id_counter;
	// [sizhuo] core id it belongs to, only valid for L1?? otherwise -1
	// XXX: what about private L2? TLB?
  int16_t coreid; 

	// [sizhuo] newly added: pointer to LSQ
	MTLSQ *mtLSQ;
	///////

  void addLowerLevel(MemObj *obj); // [sizhuo] add lower mem obj (closer to mem)
	void addUpperLevel(MemObj *obj); // [sizhuo] add upper mem obj (closer to proc)

public:
  MemObj(const char *section, const char *sName);
  MemObj();
  virtual ~MemObj();

  const char *getSection() const { return section; }
  const char *getName() const    { return name;    }
  uint16_t getID() const         { return id;      }
  int16_t getCoreID() const      { return coreid;  }
  void setCoreID(int16_t cid)    { coreid = cid;  }
	bool isFirstLevel() const { return coreid != -1; };

  MRouter *getRouter()           { return router;  }
  
  // Interface for fast-forward (no BW, just warmup caches)
  virtual TimeDelta_t ffread(AddrType addr) = 0;
  virtual TimeDelta_t ffwrite(AddrType addr) = 0;

	// [sizhuo] down: closer to memory, up: closer to core

  // DOWN
	// [sizhuo] handle msg from upper level
	
	// [sizhuo] handle upgrade req from upper level
  virtual void req(MemRequest *req) = 0;
	// [sizhuo] handle downgrade resp from upper level
  virtual void setStateAck(MemRequest *req) = 0;
	// [sizhuo] handle evicted cache line from upper level
  virtual void disp(MemRequest *req) = 0;

	// [sizhuo] these three functions do real work
  virtual void doReq(MemRequest *req) = 0;
  virtual void doSetStateAck(MemRequest *req) = 0;
  virtual void doDisp(MemRequest *req) = 0;

  // UP
	// [sizhuo] handle msg from lower level
	
	// [sizhuo] handle upgrade resp from lower level
  virtual void reqAck(MemRequest *req) = 0;
	// [sizhuo] handle downgrade req from lower level
  virtual void setState(MemRequest *req) = 0;

	// [sizhuo] these two functions do real work
  virtual void doReqAck(MemRequest *req) = 0;
  virtual void doSetState(MemRequest *req) = 0;

	// [sizhuo] mem obj is currently handling addr
	virtual bool isBusy(AddrType addr) const = 0;
  
  // Print stats
  virtual void dump() const;

  // Optional virtual methods
  virtual bool checkL2TLBHit(MemRequest *req);
  virtual void replayCheckLSQ_removeStore(DInst *);
  virtual void updateXCoreStores(AddrType addr);
  virtual void replayflush();
  virtual void setTurboRatio(float r);
  virtual void plug();

	virtual void setNeedsCoherence();
	virtual void clearNeedsCoherence();

	// [sizhuo] set LSQ pointer
	void setMTLSQ(MTLSQ *q) {
		I(mtLSQ == 0);
		mtLSQ = q;
		I(mtLSQ);
	}
};

class DummyMemObj : public MemObj {
private:
protected:
public:
  DummyMemObj();
  DummyMemObj(const char *section, const char *sName);

	// Entry points to schedule that may schedule a do?? if needed
	void req(MemRequest *req)         { doReq(req); };
	void reqAck(MemRequest *req)      { doReqAck(req); };
	void setState(MemRequest *req)    { doSetState(req); };
	void setStateAck(MemRequest *req) { doSetStateAck(req); };
	void disp(MemRequest *req)        { doDisp(req); }

	// This do the real work
	void doReq(MemRequest *req);
	void doReqAck(MemRequest *req);
	void doSetState(MemRequest *req);
	void doSetStateAck(MemRequest *req);
	void doDisp(MemRequest *req);

  TimeDelta_t ffread(AddrType addr);
  TimeDelta_t ffwrite(AddrType addr);

	bool isBusy(AddrType addr) const;
};

#endif // MEMOBJ_H
