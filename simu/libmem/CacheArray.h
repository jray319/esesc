#ifndef CACHE_ARRAY_H
#define CACHE_ARRAY_H

#include "CacheLine.h"
#include "MemRequest.h"
#include "Port.h"

// [sizhuo] base class for all cache arrays
class CacheArray {
private:
	char *name; // [sizhuo] cache array name
	// [sizhuo] contention ports for each bank
	PortGeneric **tagPort;
	PortGeneric **dataPort;

public:
  const uint32_t  size; // [sizhuo] cache size in byte
  const uint32_t  lineSize; // [sizhuo] cache line size in byte
  const uint32_t  assoc; // [sizhuo] set associativity
	const uint32_t  bankNum; // [sizhuo] number of banks

  const uint32_t  setNum; // [sizhuo] number of cache sts

  const uint32_t  log2Assoc;
  const uint32_t  log2LineSize;
  const uint32_t  maskAssoc;
  const uint32_t  maskSets; // [sizhuo] mask for index calculation
  const uint32_t  log2Sets;
	const uint32_t  maskBank;

	CacheArray(const uint32_t size_, const uint32_t lineSize_, const uint32_t assoc_, const uint32_t bankNum_, const char *name_str)
		: name(0)
		, tagPort(0)
		, dataPort(0)
		, size(size_)
		, lineSize(lineSize_)
		, assoc(assoc_)
		, bankNum(bankNum_)
		, setNum(size / lineSize / assoc)
		, log2Assoc(log2i(assoc))
		, log2LineSize(log2i(lineSize))
		, maskAssoc(assoc - 1)
		, maskSets(setNum - 1)
		, log2Sets(log2i(setNum))
		, maskBank(bankNum - 1)
	{
		I(size > 0);
		I(lineSize > 0);
		I(assoc > 0);
		I(setNum > 0);
		I(bankNum > 0);
		I((0x01L << log2i(size)) == size);
		I((0x01L << log2LineSize) == lineSize);
		I((0x01L << log2Assoc) == assoc);
		I((0x01L << log2Sets) == setNum);
		I((0x01L << log2i(bankNum)) == bankNum);

		// [sizhuo] create name
		name = new char[strlen(name_str) + 50];
		I(name);
		sprintf(name, "%s_LRU_array", name_str);

		// [sizhuo] create contention ports
		tagPort = new PortGeneric*[bankNum];
		dataPort = new PortGeneric*[bankNum];
		I(tagPort);
		I(dataPort);

		char *portName = new char[strlen(name) + 50];
		I(portName);

		// [sizhuo] fully pipelined ports
		for(uint32_t i = 0; i < bankNum; i++) {
			sprintf(portName, "%s_tag_bank[%d]", name, i);
			tagPort[i] = 0;
			tagPort[i] = PortGeneric::create(portName, 1, 1);
			I(tagPort[i]);

			sprintf(portName, "%s_data_bank[%d]", name, i);
			dataPort[i] = 0;
			dataPort[i] = PortGeneric::create(portName, 1, 1);
			I(dataPort[i]);
		}
		delete[]portName;
	}

	virtual ~CacheArray() {
		if(name) {
			delete []name;
		}
	}

	// [sizhuo] functions to get line addr & index
	AddrType getLineAddr (AddrType byteAddr) const {
		return byteAddr >> log2LineSize;
	}
	AddrType getIndex (AddrType lineAddr) const {
		return lineAddr & maskSets;
	}
	AddrType getTag (AddrType lineAddr) const {
		return lineAddr >> log2Sets;
	}
	uint32_t getBank(AddrType lineAddr) const {
		return lineAddr & maskBank;
	}

	// [sizhuo] find a valid cache line (MES) matching address
	virtual CacheLine *downReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) = 0;
	// [sizhuo] find a cache line to serve the upgrade req, update LRU states
	// if cache miss, use LRU to return the replaced line.
	virtual CacheLine *upReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) = 0;
	// [sizhuo] find cache line: this must be called before mreq is recycled
	// and is only called by up reqAck
	virtual CacheLine *upReqFindLine(AddrType lineAddr, const MemRequest *mreq) = 0;
	// [sizhuo] find a cache line for downgrade resp (setStateAck & disp)
	virtual CacheLine *downRespFindLine(AddrType lineAddr) = 0;

	// [sizhuo] port contentions
	Time_t getTagAccessTime(AddrType lineAddr, bool statsFlag) {
		return tagPort[getBank(lineAddr)]->nextSlot(statsFlag);
	}
	Time_t getDataAccessTime(AddrType lineAddr, bool statsFlag) {
		return dataPort[getBank(lineAddr)]->nextSlot(statsFlag);
	}
};

class LRUCacheArray : public CacheArray {
private:
	// [sizhuo] head of list -- LRU; tail of list -- MRU
	typedef std::list<CacheLine*> CacheSet;
	CacheSet *tags;


public:
	LRUCacheArray(const uint32_t size_, const uint32_t lineSize_, const uint32_t assoc_, const uint32_t bankNum_, const int upNodeNum, const char *name_str);
	virtual ~LRUCacheArray();

	virtual CacheLine *downReqOccupyLine(AddrType lineAddr, const MemRequest *mreq);
	virtual CacheLine *upReqOccupyLine(AddrType lineAddr, const MemRequest *mreq);
	virtual CacheLine *upReqFindLine(AddrType lineAddr, const MemRequest *mreq);
	virtual CacheLine *downRespFindLine(AddrType lineAddr);
};

#endif
