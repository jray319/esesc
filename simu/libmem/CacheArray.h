#ifndef CACHE_ARRAY_H
#define CACHE_ARRAY_H

// [sizhuo] base class for all cache arrays
class CacheArray {
protected:
  const uint32_t  size; // [sizhuo] cache size in byte
  const uint32_t  lineSize; // [sizhuo] cache line size in byte
  const uint32_t  assoc; // [sizhuo] set associativity

  const uint32_t  setNum; // [sizhuo] number of cache sts

  const uint32_t  log2Assoc;
  const uint64_t  log2LineSize;
  const uint64_t  maskAssoc;
  const uint32_t  maskSets; // [sizhuo] mask for index calculation
  const uint32_t  log2Sets;

public:
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
};

#endif
