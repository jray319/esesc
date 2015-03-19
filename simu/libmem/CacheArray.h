#ifndef CACHE_ARRAY_H
#define CACHE_ARRAY_H

// [sizhuo] MESI coherence cache line base class
class CacheLine {
public:
	typedef enum {I, S, E, M} MESI;
	MESI state; // [sizhuo] MESI state of this cache line
	MESI *dir; // [sizhuo] directory for cache line in upper level
	AddrType lineAddr;
	MemRequest *upReq; // [sizhuo] upgrade req operating on this line
	MemRequest *downReq; // [sizhuo] downgrade req operating on this line
	CacheLine() : state(I), dir(0), lineAddr(0), upReq(0), downReq(0) {}
};

// [sizhuo] base class for all cache arrays
class CacheArray {
public:
  const uint32_t  size; // [sizhuo] cache size in byte
  const uint32_t  lineSize; // [sizhuo] cache line size in byte
  const uint32_t  assoc; // [sizhuo] set associativity

  const uint32_t  setNum; // [sizhuo] number of cache sts

  const uint32_t  log2Assoc;
  const uint64_t  log2LineSize;
  const uint64_t  maskAssoc;
  const uint32_t  maskSets; // [sizhuo] mask for index calculation
  const uint32_t  log2Sets;

	CacheArray(const uint32_t sz, const uint32_t lineSz, const uint32_t a)
		: size(sz)
		, lineSize(lineSz)
		, assoc(a)
		, setNum(size / lineSize / assoc)
		, log2Assoc(log2i(assoc))
		, log2LineSize(log2i(lineSize))
		, maskAssoc(assoc - 1)
		, maskSets(setNum - 1)
		, log2Sets(log2i(setNum))
	{
		I(size > 0);
		I(lineSize > 0);
		I(assoc > 0);
		I(setNum > 0);
		I((0x01L << log2i(size)) == size);
		I((0x01L << log2LineSize) == lineSize);
		I((0x01L << log2Assoc) == assoc);
		I((0x01L << log2Sets) == setNum);
	}

	~CacheArray() {}

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
};

#endif
