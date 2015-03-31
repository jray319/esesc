// includes and copyright notice {{{1
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

#include "SescConf.h"
#include "MemorySystem.h"
#include "MemRequest.h"

#include "NICECache.h"
/* }}} */
NICECache::NICECache(MemorySystem *gms, const char *section, const char *sName)
  /* dummy constructor {{{1 */
  : MemObj(section, sName)
  ,hitDelay  (SescConf->getInt(section,"hitDelay"))
	,port(0)
  ,readHit         ("%s:readHit",         sName)
  ,pushDownHit     ("%s:pushDownHit",     sName)
  ,writeHit        ("%s:writeHit",        sName)
  ,readMiss        ("%s:readMiss",        sName)
  ,readHalfMiss    ("%s:readHalfMiss",    sName)
  ,writeMiss       ("%s:writeMiss",       sName)
  ,writeHalfMiss   ("%s:writeHalfMiss",   sName)
  ,writeExclusive  ("%s:writeExclusive",  sName)
  ,writeBack       ("%s:writeBack",       sName)
{

	// FIXME: the hitdelay should be converted to dyn_hitDelay to support DVFS
	
	I(hitDelay > 0);

	// [sizhuo] get max req in flight
	int32_t maxReqNum = SescConf->getInt(section, "maxReq");
	I(maxReqNum > 0);
	// [sizhuo] create port: num: maxReqNum, occ: hitDelay
	port = PortGeneric::create(sName, maxReqNum, hitDelay);
	I(port);

	MSG("NICE cache %s: num %d, occ %d", sName, maxReqNum, hitDelay);
}
/* }}} */

void NICECache::doReq(MemRequest *mreq)    
  /* read (down) {{{1 */
{ 
	// [sizhuo] req should retrieve data
	Time_t when = port->nextSlot(mreq->getStatsFlag()) + hitDelay;

  readHit.inc(mreq->getStatsFlag());

	if (mreq->isHomeNode()) {
		I(0); // [sizhuo] should not be used as L1
		mreq->ackAbs(when);
		return;
	}
	if (mreq->getAction() == ma_setValid || mreq->getAction() == ma_setExclusive) {
		mreq->convert2ReqAck(ma_setExclusive);
		//MSG("rdnice %x",mreq->getAddr());
	}else {
		mreq->convert2ReqAck(ma_setDirty);
		//MSG("wrnice %x",mreq->getAddr());
	}

  router->scheduleReqAckAbs(mreq, when);
}
/* }}} */

void NICECache::doReqAck(MemRequest *req)    
  /* req ack {{{1 */
{ 
  I(0);
}
// 1}}}

void NICECache::doSetState(MemRequest *req)    
  /* change state request  (up) {{{1 */
{ 
	I(0);
}
/* }}} */

void NICECache::doSetStateAck(MemRequest *req)    
  /* push (down) {{{1 */
{ 
	I(0);
}
/* }}} */

void NICECache::doDisp(MemRequest *req)    
  /* push (up) {{{1 */
{ 
  writeBack.inc(req->getStatsFlag());
	// [sizhuo] schedule write
	Time_t when = port->nextSlot(req->getStatsFlag());
  req->ackAbs(when);
}
/* }}} */

bool NICECache::isBusy(AddrType addr) const
  /* can accept reads? {{{1 */
{ 
  return false;  
}
/* }}} */

TimeDelta_t NICECache::ffread(AddrType addr)
  /* warmup fast forward read {{{1 */
{ 
  return 1;
}
/* }}} */

TimeDelta_t NICECache::ffwrite(AddrType addr)
  /* warmup fast forward writed {{{1 */
{ 
  return 1;
}
/* }}} */


