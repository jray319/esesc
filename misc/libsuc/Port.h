/* 
   ESESC: Super ESCalar simulator
   Copyright (C) 2003 University of Illinois.

   Contributed by Jose Renau
                  Basilio Fraguela

This file is part of ESESC.

ESESC is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2, or (at your option) any later version.

ESESC is    distributed in the  hope that  it will  be  useful, but  WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should  have received a copy of  the GNU General  Public License along with
ESESC; see the file COPYING.  If not, write to the  Free Software Foundation, 59
Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifndef PORT_H
#define PORT_H

// Clean and Fast interface to implement any kind of contention for a
// bus.  This resource is typically used to model the number of ports
// in a cache, or the occupancy of a functional unit.
//
// A real example of utilization is in Resource.cpp, but this example
// gives you a quick idea:
//
// You want to model the contention for a bus. The bus has an
// occupancy of 2 cycles. To create the port:
// 
// PortGeneric *bus = PortGeneric::create("portName",1,2);
// 1 is for the number of busses, 2 for the occupancy.
//
// At any time, you can get queued for the bus, and this structure
// tells you the next cycle when your request would be satisfied.
// Time_t atWhatCycle = bus->nextSlot();
//
// Enjoy!

#include "GStats.h"

#include "nanassert.h"
#include "callback.h"

typedef uint16_t NumUnits_t;

//! Generic Port used to model contention 
//! Based on the PortGeneric there are several types of ports. 
//! Each has a different algorithm, so that is quite fast.
class PortGeneric {
private:
  int32_t nUsers;

protected:
  GStatsAvg avgTime;
  
public:
  PortGeneric(const char *name);
  virtual ~PortGeneric();

  void subscribe() {  nUsers++; }
  void unsubscribe(){ nUsers--; }

  // [sizhuo] what is "en" doing? From .cpp, it seems only relates to stats
  // nextSlot will occupy the port and return the time when the req is served (port is occupied)
  // calNextSlot only return time but don't serve req

  TimeDelta_t nextSlotDelta(bool en) { return nextSlot(en) - globalClock; }
  //! occupy a time slot in the port. 
  //! Returns when the slot started to be occupied
  virtual Time_t nextSlot(bool en) = 0;

  //! occupy the port for a number of slots. 
  //! Returns the time that the first slot was allocated.
  //!
  //! This function is equivalent to:
  //! Time_t t = nextSlot();
  //! for(int32_t i=1;i<nSlots;i++) {
  //!  nextSlot();
  //! }
  //! return t;

  //! returns when the next slot can be free without occupying any slot
  virtual Time_t calcNextSlot() const =0;

  // [sizhuo] creat a port.
  // nUnits: max number of in flight req
  // occ: occupation time of a req
  // occupation time is different from delay, a fully pipelined port may have long delay
  // In simu.conf, there are separate parameters for occupation time and delay
  static PortGeneric *create(const char *name, 
           NumUnits_t nUnits, 
           TimeDelta_t occ);
  void destroy();
};

// [sizhuo] unlimited capacity of a port
// the port can serve unlimited number of req in one cycle
class PortUnlimited : public PortGeneric {
  Time_t lTime;
public:
  PortUnlimited(const char *name);
  
  Time_t nextSlot(bool en);
  Time_t calcNextSlot() const;
};

// [sizhuo] fully pipelined port, can serve exactly one req per cycle
class PortFullyPipe : public PortGeneric {
private:
protected:
  // lTime is the cycle in which the latest use began
  Time_t lTime; 
public:
  PortFullyPipe(const char *name);

  Time_t nextSlot(bool en);
  Time_t calcNextSlot() const;
};

// [sizhuo] fully pipelined, with N ports
class PortFullyNPipe : public PortGeneric {
private:
protected:
  const NumUnits_t nUnitsMinusOne;
  NumUnits_t freeUnits;
  Time_t lTime;
public:
  PortFullyNPipe(const char *name, NumUnits_t nFU);

  Time_t nextSlot(bool en);
  Time_t calcNextSlot() const;
};

// [sizhuo] a single blocking port
class PortPipe : public PortGeneric {
private:
protected:
  const  TimeDelta_t ocp; // [sizhuo] fixed occupation time of a req
  Time_t lTime;
public:
  PortPipe(const char *name, TimeDelta_t occ);

  Time_t nextSlot(bool en);
  Time_t calcNextSlot() const;
};

// [sizhuo] N blocking ports
class PortNPipe : public PortGeneric {
private:
protected:
  const TimeDelta_t ocp;
  const NumUnits_t nUnits;
  Time_t  *portBusyUntil;
public:
  PortNPipe(const char *name, NumUnits_t nFU, TimeDelta_t occ);
  virtual ~PortNPipe();

  Time_t nextSlot(bool en);
  Time_t nextSlot(int32_t occupancy, bool en);
  Time_t calcNextSlot() const;
};

#endif // PORT_H
