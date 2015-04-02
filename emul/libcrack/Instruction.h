// Contributed by Jose Renau
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

#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "nanassert.h"

#include <stdint.h>
#include <algorithm>
#include <vector>

#include "InstOpcode.h"

// [sizhuo] Instruction class is just an inst after decode

class Instruction {
private:
protected:
  InstOpcode  opcode;
  RegType     src1;
  RegType     src2;
  RegType     dst1;
  RegType     dst2;
  
  public:

  static const char *opcode2Name(InstOpcode type);
  void set(InstOpcode op, RegType src1, RegType src2, RegType dst1, RegType dst2, bool useImm);
  bool doesJump2Label() const  { return opcode == iBALU_LBRANCH || opcode == iBALU_LJUMP || opcode == iBALU_LCALL; }
  InstOpcode getOpcode() const { return opcode; }
  void forcemult() {opcode = iCALU_FPMULT; }

  RegType getSrc1() const { return src1;  }
  RegType getSrc2() const { return src2;  }
  RegType getDst1() const { return dst1;  }
  RegType getDst2() const { return dst2;  }

  // if dst == Invalid -> dst2 == invalid
  bool hasDstRegister() const { return dst1 != LREG_InvalidOutput || dst2 != LREG_InvalidOutput; }
  bool hasSrc1Register() const { return src1 != LREG_NoDependence;  }
  bool hasSrc2Register() const { return src2 != LREG_NoDependence;  }

  bool hasImm() const { I(0); return false; };

  bool isFuncCall() const { return opcode == iBALU_RCALL   || opcode == iBALU_LCALL;   }
  bool isFuncRet()  const { return opcode == iBALU_RET;    }
  // All the conditional control flow instructions are branches
  bool isBranch()   const { return opcode == iBALU_RBRANCH || opcode == iBALU_LBRANCH; }
  // All the unconditional but function return are jumps
  bool isJump()     const { return opcode == iBALU_RJUMP   || opcode == iBALU_LJUMP || isFuncCall(); }

  bool isControl()  const { 
    GI(opcode >= iBALU_LBRANCH && opcode <= iBALU_RET, isJump() || isBranch() || isFuncCall() || isFuncRet());

    return opcode >= iBALU_LBRANCH && opcode <= iBALU_RET; 
  }

  bool isLoad() const         { return opcode == iLALU_LD; }
  bool isStore() const        { return opcode == iSALU_ST; }
  bool isStoreAddress() const { return opcode == iSALU_ADDR; }

  bool isMemory() const   { return opcode == iSALU_ST || opcode == iLALU_LD; }

  void dump(const char *str) const;

	// [sizhuo] overload == 
	bool operator==(const Instruction& x) const {
		return opcode == x.opcode && src1 == x.src1 && src2 == x.src2 && dst1 == x.dst1 && dst2 == x.dst2;
	}
	// [sizhuo] copy
	void copy(const Instruction *x) {
		opcode = x->opcode;
		src1 = x->src1;
		src2 = x->src2;
		dst1 = x->dst1;
		dst2 = x->dst2;
	}
};

#endif   // INSTRUCTION_H

