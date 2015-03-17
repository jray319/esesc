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

#ifndef MROUTER_H
#define MROUTER_H

#include "estl.h"
#include <vector>

#include "Snippets.h"
#include "RAWDInst.h"
#include "Resource.h"

#include "nanassert.h"

/* }}} */

class MemObj;
class MemRequest;

// MsgAction enumerate {{{1
//
// MOESI States:
//
// M: Single copy, memory not consistent
//
// E: Single copy, memory is consistent
//
// I: invalid
//
// S: one of (potentially) several copies. Share does not respond to other bus snoop reads
// 
// O: Like shared, but the O is responsible to update memory. If O does
// a write back, it can change to S
//
enum MsgAction {
	ma_setInvalid,
	ma_setValid,
	ma_setDirty,
	ma_setShared,
	ma_setExclusive,
	ma_MMU,
	ma_VPCWU,
	ma_MAX
};

// }}}

// [sizhuo] MRouter is a (logical) router for one mem obj
class MRouter {
private:
protected:
  class MemObjHashFunc {
    public:
      size_t operator()(const MemObj *mobj) const {
        HASH<const char *> H;
        return H((const char *)mobj);
      }
  };
  
  MemObj *self_mobj;

  typedef HASH_MAP<const MemObj *, MemObj *, MemObjHashFunc> UPMapType;
  UPMapType up_map;
  std::vector<MemObj *> up_node;
  std::vector<MemObj *> down_node;

  void updateRouteTables(MemObj *upmobj, MemObj * const top_node);

public:
  MRouter(MemObj *obj);
  virtual ~MRouter();

	// [sizhuo] find creator of mreq among up_node[], return the index in up_node[] array
	// if not found, return -1 (i.e. mreq is created by node not in up_node
  int16_t getCreatorPort(const MemRequest *mreq) const;

  void fillRouteTables();
  void addUpNode(MemObj *upm);
  void addDownNode(MemObj *upm);

	// [sizhuo] schedule upgrade req to lower level
	// Pos version req to down_node[pos], other version req to down_node[0]
  void scheduleReqPos(     uint32_t pos, MemRequest *mreq    , TimeDelta_t lat=0);
  void scheduleReq(                      MemRequest *mreq    , TimeDelta_t lat=0);

	// [sizhuo] schedule upgrade resp to upper level
	// Pos version resp to up_node[pos]
	// other versions find the home node of original req to resp
  void scheduleReqAck(                   MemRequest *mreq    , TimeDelta_t lat=0);
  void scheduleReqAckAbs(                MemRequest *mreq    , Time_t w);
  void scheduleReqAckPos(  uint32_t pos, MemRequest *mreq    , TimeDelta_t lat=0);

	// [sizhuo] schedule downgrade req to upper_node[pos]
  void scheduleSetStatePos(uint32_t pos, MemRequest *mreq    , TimeDelta_t lat=0);
	// [sizhuo] schedule downgrade resp to lower level
	// pos version resp to down_node[pos], other version to down_node[0]
  void scheduleSetStateAck(              MemRequest *mreq    , TimeDelta_t lat=0);
  void scheduleSetStateAckPos(uint32_t pos, MemRequest *mreq    , TimeDelta_t lat=0);

	// [sizhuo] schedule eviction (voluntary downgrade resp) to lower level
	// pos version resp to down_node[pos], other version to down_node[0]
  void scheduleDispPos(    uint32_t pos, MemRequest *mreq    , TimeDelta_t lat=0);
  void scheduleDisp(                     MemRequest *mreq    , TimeDelta_t lat=0);
	// [sizhuo] starts a new eviction resp to down_node[0]
	void sendDisp(AddrType addr, bool doStats, TimeDelta_t lat=0);

	// [sizhuo] starts (downgrade) req to all upper nodes other than home node of mreq
  int32_t sendSetStateOthers(                 MemRequest *mreq, MsgAction ma, TimeDelta_t lat=0);
	// [sizhuo] starts (downgrade) req to upper_node[pos]
  int32_t sendSetStateOthersPos(uint32_t pos, MemRequest *mreq, MsgAction ma, TimeDelta_t lat=0);
	// [sizhuo] starts (downgrade) req to all upper nodes
  int32_t sendSetStateAll(MemRequest *mreq, MsgAction ma, TimeDelta_t lat=0);

	// Added by Sizhuo
	// [sizhuo] starts (downgrade) req to all upper nodes with specific addr
	// but let setState req all linked to mreq (by addPendingSetStateAck())
  int32_t invalidateAll(AddrType addr, MemRequest *mreq, TimeDelta_t lat=0);
	/////////

  TimeDelta_t ffread(AddrType addr);
  TimeDelta_t ffwrite(AddrType addr);
  TimeDelta_t ffreadPos(uint32_t pos, AddrType addr);
  TimeDelta_t ffwritePos(uint32_t pos, AddrType addr);

	// [sizhuo] is down_node[pos] busy with addr now
  bool isBusyPos(uint32_t pos, AddrType addr) const;

  bool isTopLevel() const { return up_node.empty(); }

  MemObj *getDownNode() const {
    I(down_node.size()==1);
    return down_node[0];
  }
};

#endif
