#ifndef WRAPPER_H
#define WRAPPER_H

// Contributed by Elnaz Ebrahimi
//                Jose Renau
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

/*******************************************************************************
Description: Wrapper class structure creates the interface between McPAT area,
power, and timing model and the eSESC performance simulator.
********************************************************************************/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <stdlib.h>

#include "parameter.h" // eka, to be able to refer to RunTimeParameters
#include "XML_Parse.h"
#include "processor.h"

/* }}} */

class ChipEnergyBundle;


class Wrapper {
private:
  ParseXML *p;
  Processor *proc;
  uint32_t totalPowerSamples;
  const char* pwrlogFilename;
  const char* pwrsection;
  FILE *logpwr;
  bool dumppwth;

public: 
  Wrapper();
  ~Wrapper();

  FILE *logprf;
  int cntr_count;
  uint32_t nPowerCall;

  std::map<std::string, int> mcpat_map;
  typedef std::map<std::string, int>::iterator MapIter;
  std::pair<std::map<std::string, int>::iterator, bool> ret;

  std::map<int,int> stats_map;

  void plug(const char* section, 
      std::vector<uint32_t> *statsVector,  
			ChipEnergyBundle *energyBundle,
      uint32_t *coreEIdx, uint32_t *ncores,
      uint32_t *nL2, uint32_t *nL3,
      std::vector<uint32_t> *coreIndex,
      std::vector<uint32_t> *gpuIndex,
			bool display);
  //deallocate structures, empty for now     
  void unplug() { }

  void calcPower(vector<uint32_t> *statsVector, ChipEnergyBundle *energyBundle, std::vector<uint64_t> *clockInterval);
};

#endif

