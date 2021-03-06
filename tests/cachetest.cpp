//cache test
//
// this file tests that the caches adhere to the MOESI protocol as described
// by AMD which can be found here:
// http://www.chip-architect.com/news/2003_09_21_Detailed_Architecture_of_AMDs_64bit_Core.html#3.18:
//
//possible state transitions:
// 
// Modified-> Owner
// Modified-> Invalid
//
// Exclusive-> Modified
// Exclusive-> Shared
// Exclusive-> Invalid
//
// Owner-> Invalid
//
// Shared-> Modified
// Shared-> Invalid
//
// Invalid-> Modified
// Invalid-> Exclusive
// Invalid-> Shared
//
//
//

#include <stdio.h>
#include <exception>
#include "gtest/gtest.h"
#include "CCache.h"
#include "GProcessor.h"
#include "BootLoader.h"
#include "MemObj.h"
#include "MemRequest.h"
#include "callback.h"
#include "MemorySystem.h"
#include "SescConf.h"
#include "DInst.h"
#include "nanassert.h"
#include "RAWDInst.h"
#include "Instruction.h"
#include "CrackBase.h"
#include "ARMCrack.h"
#include "ThumbCrack.h"
#include "MemStruct.h"

using ::testing::EmptyTestEventListener;
using ::testing::InitGoogleTest;
using ::testing::Test;
using ::testing::TestCase;
using ::testing::TestEventListeners;
using ::testing::TestInfo;
using ::testing::TestPartResult;
using ::testing::UnitTest;

static int rd_pending = 0;
static int wr_pending = 0;
static int num_operations=0;

enum currState{
  Modified, //0
  Exclusive, //1
  Owner, //2
  Shared,//3
  Invalid //4
};

currState getState(CCache *cache, AddrType addr){
  currState state;

  if(cache->Modified(addr))
    state=Modified;

  if(cache->Exclusive(addr))
    state=Exclusive;
  
  if(cache->Owner(addr))
    state=Owner;

  if(cache->Shared(addr))
    state=Shared;

  if(cache->Invalid(addr))
      state=Invalid;

  return state;
}

DInst *ld;
DInst *st;
ARMCrack crackInstARM;

RAWDInst rinst;

void rdDone(DInst *dinst) {

  printf("rddone @%lld\n",(long long)globalClock);

	rd_pending--;
  dinst->recycle();
}

void wrDone(DInst *dinst) {

  printf("wrdone @%lld\n",(long long)globalClock);
	wr_pending--;
  dinst->recycle();
}

static void waitAllMemOpsDone() {
  while(rd_pending || wr_pending)
    EventScheduler::advanceClock();
}

typedef CallbackFunction1<DInst *, &rdDone> rdDoneCB;
typedef CallbackFunction1<DInst *, &wrDone> wrDoneCB;

static void doread(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *ldClone = ld->clone();
  ldClone->setAddr(addr);

  while(cache->isBusy(addr))
    EventScheduler::advanceClock();

  rdDoneCB *cb = rdDoneCB::create(ldClone);
  printf("rd %x @%lld\n", (unsigned int)addr,(long long)globalClock);
  MemRequest::sendReqRead(cache, ldClone, addr, cb);
  rd_pending++;
}

static void dowrite(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *stClone = st->clone();
  stClone->setAddr(addr);

	while(cache->isBusy(addr))
		EventScheduler::advanceClock();

  wrDoneCB *cb = wrDoneCB::create(stClone);
  printf("wr %x @%lld\n", (unsigned int)addr,(long long)globalClock);
	MemRequest::sendReqWrite(cache, stClone, addr, cb);
	wr_pending++;
}

bool pluggedin = false;
GMemorySystem *gms_p0 = 0;
GMemorySystem *gms_p1 = 0;
void initialize(){
  
  if(!pluggedin){
#if 0
    BootLoader::plug(arg1,arg2);
#else
    int arg1          = 1;
    const char *arg2[] = {"cachetest", 0};
    setenv("ESESC_tradCORE_DL1","DL1_core DL1",1);

    SescConf = new SConfig(arg1, arg2);
    unsetenv("ESESC_tradCore_DL1");
    gms_p0 = new MemorySystem(0);
    gms_p0->buildMemorySystem();
    gms_p1 = new MemorySystem(1);
    gms_p1->buildMemorySystem();
#endif
    pluggedin=true;
  }

  // Create a LD (e5d33000) with PC = 0xfeeffeef and address 1203
  rinst.set(0xe5d33000,0xfeeffeef,1203,true);
  crackInstARM.expand(&rinst);
  ld = DInst::create(rinst.getInstRef(0), &rinst, rinst.getPC(), 0);

  // Create a ST (e5832000) with PC = 0x410 and address 0x400
  rinst.set(0xe5832000,0x410,0x400,true);
  crackInstARM.expand(&rinst);
  st = DInst::create(rinst.getInstRef(0), &rinst, rinst.getPC(), 0);
}


CCache *p0getDL1(){
#if 0
  GProcessor *gproc0 = TaskHandler::getSimu(0);
  MemObj *P0DL1=gproc0->getMemorySystem()->getDL1();
#else
  MemObj *P0DL1=gms_p0->getDL1();
#endif
  CCache *p0cl1=static_cast<CCache*>(P0DL1);
  //p0cl1->setNeedsCoherence();
  return p0cl1;
}

CCache *p0getL2(){
#if 0
	GProcessor *gproc0 = TaskHandler::getSimu(0);
	MemObj *P0DL1=gproc0->getMemorySystem()->getDL1();
#else
	MemObj *P0DL1=gms_p0->getDL1();
#endif
	MRouter *router=P0DL1->getRouter();
	MemObj *L2=router->getDownNode();
	 CCache *l2c=static_cast<CCache*>(L2);
	//l2c->setNeedsCoherence();
   return l2c;
}
CCache *p0getL3(){
#if 0
	GProcessor *gproc0 = TaskHandler::getSimu(0);
	MemObj *P0DL1=gproc0->getMemorySystem()->getDL1();
#else
	MemObj *P0DL1=gms_p0->getDL1();
#endif
	MRouter *router=P0DL1->getRouter();
	MemObj *L2=router->getDownNode();
	MRouter *router2= L2->getRouter();
  MemObj *L3=router2->getDownNode();
	CCache *l3c= static_cast<CCache*>(L3);
  //l3c->setNeedsCoherence();
  return l3c;
}


CCache *p1getDL1(){
#if 0
	GProcessor *gproc1 = TaskHandler::getSimu(1);
	MemObj *P1DL1=gproc1->getMemorySystem()->getDL1();
#else
	MemObj *P1DL1=gms_p1->getDL1();
#endif
  CCache *p1d1=static_cast<CCache*>(P1DL1);
  //p1d1->setNeedsCoherence();
	return p1d1;
}

CCache *p1getL2() {
#if 0
	GProcessor *gproc1 = TaskHandler::getSimu(1);
	MemObj *P1DL1=gproc1->getMemorySystem()->getDL1();
#else
	MemObj *P1DL1=gms_p1->getDL1();
#endif
	MRouter *router=P1DL1->getRouter();
	MemObj *L2=router->getDownNode();
  CCache *cL2=static_cast<CCache*>(L2);
  //cL2->setNeedsCoherence();
  return cL2;
}


class CacheTest:public testing::Test{
protected:
  CCache *p0dl1;
  CCache *p0l2;
  CCache *p1l2;
  CCache *l3;
  CCache *p1dl1;
  virtual void SetUp() {
    initialize();
    p0dl1=p0getDL1();
    p0l2 =p0getL2();
    p1l2 =p1getL2();
    l3   =p0getL3();
    p1dl1=p1getDL1();
  }
  virtual void TearDown() {
    BootLoader::unplug();
  }
};

// the first test checks that if no cache has data and then one 
// cache reads it from memory, then that cache gets set to Exclusive

TEST_F(CacheTest,Exclusive_first_test){
  doread(p0dl1,0x100);
  waitAllMemOpsDone();
  EXPECT_EQ(Exclusive,getState(p0dl1,0x100));
  EXPECT_EQ(Invalid,getState(p1dl1,0x100));
  EXPECT_EQ(Exclusive,getState(p0l2,0x100));
  EXPECT_EQ(Exclusive,getState(l3,0x100));
}

// the second test checks that if one cache reads a line and then
// a second cache reads that line,  both become shared
TEST_F(CacheTest,Shared_second_test){
  doread(p0dl1,0x200);
  waitAllMemOpsDone();
  doread(p1dl1,0x200);
  waitAllMemOpsDone();
  
  EXPECT_EQ(Shared,getState(p0dl1,0x200));
  EXPECT_EQ(Shared,getState(p1dl1,0x200));
  EXPECT_EQ(Shared,getState(p0l2,0x200));
  EXPECT_EQ(Shared,getState(p1l2,0x200));
  if (p0l2 == p1l2) { // Shared L2 conf
    // The request did not reach the L3
    EXPECT_EQ(Exclusive,getState(l3,0x200));
  }else{
    EXPECT_EQ(Shared,getState(l3,0x200));
  }
}

// in the third test, a cache has a write miss that no one else has, and we check if 
// the state of that cache goes to modified

TEST_F(CacheTest, Modified_third_test){
  dowrite(p0dl1,0x300);
  waitAllMemOpsDone();
  EXPECT_EQ(Modified,getState(p0dl1,0x300));
  EXPECT_EQ(Exclusive,getState(p0l2,0x300));
  EXPECT_EQ(Exclusive,getState(l3,0x300));
}

// in the fourth test, a cache reads from memory and becomes exclusive, 
// then has a write hit and should go to modified
//
// currently this test fails, and it stays at exclusive
//
TEST_F(CacheTest,Modified_fourth_test){
  doread(p0dl1,0x400);
  waitAllMemOpsDone();
  dowrite(p0dl1,0x400);
  waitAllMemOpsDone();
  EXPECT_EQ(Modified,getState(p0dl1,0x400));
  EXPECT_EQ(Exclusive,getState(p0l2,0x400));
  EXPECT_EQ(Exclusive,getState(l3,0x400));
}

// in the fifth test, a cache has a write miss and goes to modified, then
// another cache has a read miss, so the first one becomes owner and the 
// second becomes shared
//
TEST_F(CacheTest,Owner_fifth_test){
  dowrite(p0dl1,0x500);
  waitAllMemOpsDone();
  doread(p1dl1,0x500);
  waitAllMemOpsDone();

  EXPECT_EQ(Owner, getState(p0dl1,0x500));
  EXPECT_EQ(Shared,getState(p1dl1,0x500));
  EXPECT_EQ(Shared,getState(p0l2,0x500));
}

//in the sixth test, a cache has a read miss, then the other
//cache has a write miss, invalidating the first cache and becoming modified
//itself
//
//The test currently passes, but it should be noted that the cache line does
//not actually enter an invalid state, it is just assigned a null pointer
TEST_F(CacheTest, Invalid_sixth_test){
  doread(p0dl1,0x600);
  waitAllMemOpsDone();
  dowrite(p1dl1,0x600);
  waitAllMemOpsDone();

  EXPECT_EQ(Invalid,getState(p0dl1,0x600));
  EXPECT_EQ(Modified,getState(p1dl1,0x600));
  EXPECT_EQ(Exclusive,getState(p1l2,0x600));
  EXPECT_EQ(Exclusive,getState(l3,0x600));
}


//in the seventh test, a cache has a read miss and then the other cache has
//a read miss, so then they are both shared. Then the second cache does a 
//write and becomes modified, while the first cache is invalidated.

TEST_F(CacheTest,Invalid_seventh_test){
  doread(p0dl1,0x700);
  waitAllMemOpsDone();
  EXPECT_EQ(Exclusive,getState(p0dl1,0x700));
  doread(p1dl1,0x700);
  waitAllMemOpsDone();
  EXPECT_EQ(Shared,getState(p0dl1,0x700));
  EXPECT_EQ(Shared,getState(p1dl1,0x700));
  EXPECT_EQ(Shared,getState(p0l2,0x700));
  EXPECT_EQ(Shared,getState(p1l2,0x700));
  if (p0l2 == p1l2) { // Shared L2 conf
    EXPECT_EQ(Exclusive,getState(l3,0x700));
  }else{
    EXPECT_EQ(Shared,getState(l3,0x700));
  }
  dowrite(p1dl1,0x700);
  waitAllMemOpsDone();

  EXPECT_EQ(Invalid,getState(p0dl1,0x700));
  EXPECT_EQ(Modified,getState(p1dl1,0x700));
  EXPECT_EQ(Exclusive,getState(p1l2,0x700));
  EXPECT_EQ(Exclusive,getState(l3,0x700));
}

//in the eighth test, a cache has a write miss, becoming modified. Then 
//another cache has a read miss, so that the first one becomes owner
//and the second becomes shared. Then the second one is written to becoming
//Modified, invalidating the owner
//
//This test fails. The first cache does transition from Owner to Invalid,
//but the second cache fails to go from shared to modified.
TEST_F(CacheTest,Invalid_eighth_test){
  dowrite(p1dl1,0x800);
  waitAllMemOpsDone();
  doread(p0dl1,0x800);
  waitAllMemOpsDone();
  EXPECT_EQ(Owner,getState(p1dl1,0x800));
  EXPECT_EQ(Shared,getState(p0dl1,0x800));
  dowrite(p0dl1,0x800);
  waitAllMemOpsDone();

  EXPECT_EQ(Invalid,getState(p1dl1,0x800));
  EXPECT_EQ(Modified,getState(p0dl1,0x800));
  EXPECT_EQ(Exclusive,getState(p0l2,0x800));
  EXPECT_EQ(Exclusive,getState(l3,0x800));
}
 

//in the ninth test, a cache does a read to become exclusive. Then another 
//cache does a write which makes it modified and the first cache invalid
//
TEST_F(CacheTest,Invalid_ninth_test){
  doread(p0dl1,0x900);
  waitAllMemOpsDone();
  dowrite(p1dl1,0x900);
  waitAllMemOpsDone();
  EXPECT_EQ(Invalid,getState(p0dl1,0x900));
  EXPECT_EQ(Modified,getState(p1dl1,0x900));
  EXPECT_EQ(Exclusive,getState(p1l2,0x900));
  EXPECT_EQ(Exclusive,getState(l3,0x900));

  doread(p1dl1,0x900);
  waitAllMemOpsDone();
  EXPECT_EQ(Invalid,getState(p0dl1,0x900));
  EXPECT_EQ(Modified,getState(p1dl1,0x900));
  EXPECT_EQ(Exclusive,getState(p1l2,0x900));
  EXPECT_EQ(Exclusive,getState(l3,0x900));

  doread(p0dl1,0x900);
  waitAllMemOpsDone();
  EXPECT_EQ(Shared,getState(p0dl1,0x900));
  EXPECT_EQ(Owner,getState(p1dl1,0x900));
  EXPECT_EQ(Shared,getState(p0l2,0x900));
  if (p0l2 == p1l2) { // Shared L2 conf
    EXPECT_EQ(Exclusive,getState(l3,0x900));
  }else{
    EXPECT_EQ(Shared,getState(l3,0x900));
  }

  dowrite(p0dl1,0x900);
  waitAllMemOpsDone();
  EXPECT_EQ(Invalid,getState(p1dl1,0x900));
  EXPECT_EQ(Modified,getState(p0dl1,0x900));
  EXPECT_EQ(Exclusive,getState(p0l2,0x900));
  EXPECT_EQ(Exclusive,getState(l3,0x900));
}


