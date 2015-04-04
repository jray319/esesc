/*
   ESESC: Super ESCalar simulator
   Copyright (C) 2010 University of California, Santa Cruz.

   Contributed by Luis Ceze

This file is part of ESESC.

ESESC is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2, or (at your option) any later version.

ESESC is distributed in the  hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
ESESC; see the file COPYING. If not, write to the Free Software Foundation, 59
Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/


#ifndef LDSTQ_H
#define LDSTQ_H

#include <vector>
#include <set>
#include <map>

#include "estl.h"
#include "GStats.h"

#include "DInst.h"
#include "Resource.h"

class LSQ {
private:
protected:

  LSQ() {}

  virtual ~LSQ() { }

public:

  virtual void insert(DInst *dinst)      = 0; // [sizhuo] add inst into LSQ
  virtual DInst *executing(DInst *dinst) = 0; // [sizhuo] ex an inst?
  virtual void remove(DInst *dinst)      = 0; // [sizhuo] remove inst from LSQ

	// [sizhuo] newly added functions
	virtual StallCause addEntry(DInst *dinst) = 0;
	virtual void issue(DInst *dinst) = 0;
	typedef CallbackMember1<LSQ, DInst*, &LSQ::issue> issueCB;
	virtual void retire(DInst *dinst) = 0;
	virtual bool isComSQEmpty() = 0;
};

// [sizhuo] LSQ for OOO core
class LSQFull : public LSQ {
private:

  class AddrTypeHashFunc {
    public: 
	  // [sizhuo] DataType is uint64
      size_t operator()(const DataType p) const {
        return((uint64_t) p);
      }
  };

  // [sizhuo] map from addrType (uint64) to DInst*, i.e. from load/store address to uOP
  // It's possible that 1 load/store addr is mapped to multiple inst
  typedef HASH_MULTIMAP<AddrType, DInst *, AddrTypeHashFunc> AddrDInstQMap;

  GStatsCntr    stldForwarding;
  AddrDInstQMap instMap;

  // [sizhuo] memory access is word aligned, truncate last 2 bits in addr
  static AddrType calcWord(const DInst *dinst) {
    return (dinst->getAddr()) >> 2;
  }

public:
  LSQFull(const int32_t id);
  ~LSQFull() { }

  void insert(DInst *dinst);
  DInst *executing(DInst *dinst);
  void remove(DInst *dinst);

	// [sizhuo] newly added functions
	virtual StallCause addEntry(DInst *dinst) { return NoStall; }
	virtual void issue(DInst *dinst) {}
	virtual void retire(DInst *dinst) {}
	virtual bool isComSQEmpty() { return true; }
};

// [sizhuo] an dummy LSQ, used for in order core
class LSQNone : public LSQ {
private:

public:
  LSQNone(const int32_t id);
  ~LSQNone() { }

  void insert(DInst *dinst);
  DInst *executing(DInst *dinst);
  void remove(DInst *dinst);

	// [sizhuo] newly added functions
	virtual StallCause addEntry(DInst *dinst) { return NoStall; }
	virtual void issue(DInst *dinst) {}
	virtual void retire(DInst *dinst) {}
	virtual bool isComSQEmpty() { return true; }
};

class LSQVPC : public LSQ {
private:
  std::multimap<AddrType, DInst*> instMap;

  GStatsCntr LSQVPC_replays;

  static AddrType calcWord(const DInst *dinst) {
    return (dinst->getAddr()) >> 2;
  }

public:
  LSQVPC();
  ~LSQVPC() { }

  void insert(DInst *dinst);
  DInst * executing(DInst *dinst);
  void remove(DInst *dinst);
  AddrType replayCheck(DInst *dinst);

	// [sizhuo] newly added functions
	virtual StallCause addEntry(DInst *dinst) { return NoStall; }
	virtual void issue(DInst *dinst) {}
	virtual void retire(DInst *dinst) {}
	virtual bool isComSQEmpty() { return true; }
};
#endif
