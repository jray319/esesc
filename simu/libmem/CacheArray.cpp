#include "CacheArray.h"
#include <queue>
#include "nanassert.h"
#include "MemRequest.h"
#include <string.h>

LRUCacheArray::LRUCacheArray(const uint32_t size_, const uint32_t lineSize_, const uint32_t assoc_, const uint32_t bankNum_, const int upNodeNum, const char *name_str)
	: CacheArray(size_, lineSize_, assoc_, bankNum_, name_str)
	, tags(0)
{
	// [sizhuo] create tag arrays
	tags = new CacheSet[setNum];
	I(tags);
	for(uint32_t i = 0; i < setNum; i++) {
		for(uint32_t j = 0; j < assoc; j++) {
			CacheLine *line = new CacheLine(upNodeNum);
			tags[i].push_back(line);
		}
	}
}

LRUCacheArray::~LRUCacheArray() {
	if(tags) {
		for(uint32_t i = 0; i < setNum; i++) {
			while(!tags[i].empty()) {
				CacheLine *line = tags[i].front();
				tags[i].pop_front();
				I(line);
				delete line;
			}
		}
		delete[]tags;
	}
}

CacheLine* LRUCacheArray::downReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) {
	const AddrType index = getIndex(lineAddr);
	CacheLine *line = 0;
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->state != CacheLine::I) {
			// [sizhuo] MSHR ensures that this line can only be occupied by up req in Wait state
			line = *iter;
			I(line->downReq == 0);
			line->downReq = mreq;
			return line;
		}
	}
	return 0;
}

CacheLine* LRUCacheArray::upReqOccupyLine(AddrType lineAddr, const MemRequest *mreq) {
	const AddrType index = getIndex(lineAddr);
	CacheLine *line = 0;

	// [sizhuo] MSHR ensures that the occupied line in this function will not be occupied by other req

	// [sizhuo] find hit line
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->state != CacheLine::I) {
			// [sizhuo] MSHR ensures that this line is not being replaced/downgraded by other req
			line = (*iter);
			// [sizhuo] promote line to MRU
			tags[index].erase(iter);
			tags[index].push_back(line);
			// [sizhuo] occupy & return
			I(line->downReq == 0);
			I(line->upReq == 0);
			line->upReq = mreq;
			return line;
		}
	}

	// [sizhuo] try to find invalid line
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->state == CacheLine::I && (*iter)->downReq == 0 && (*iter)->upReq == 0) {
			line = (*iter);
			// [sizhuo] promote line to MRU
			tags[index].erase(iter);
			tags[index].push_back(line);
			// [sizhuo] occupy & return
			line->upReq = mreq;
			return line;
		}
	}

	// [sizhuo] replace the LRU line
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->downReq == 0 && (*iter)->upReq == 0) {
			line = (*iter);
			// [sizhuo] promote line to MRU
			tags[index].erase(iter);
			tags[index].push_back(line);
			// [sizhuo] occupy & return
			line->upReq = mreq;
			return line;
		}
	}

	// [sizhuo] fail to occupy a line, error
	MSG("ERROR: upgrade req fail to occupy a line %lx", lineAddr);
	int i = 0;
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++, i++) {
		MSG("tags[%lx][%d]: state %d, lineAddr %lx, upReq %p, downReq %p", index, i, (*iter)->state, (*iter)->lineAddr, (*iter)->upReq, (*iter)->downReq);
	}	
	return 0;
}

CacheLine* LRUCacheArray::upReqFindLine(AddrType lineAddr, const MemRequest *mreq) {
	const AddrType index = getIndex(lineAddr);
	I(lineAddr == getLineAddr(mreq->getAddr()));
	I(mreq->isReqAck());

	// [sizhuo] when reqAck comes, cache line should not have down req
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->upReq == mreq && (*iter)->downReq == 0) {
			return (*iter);
		}
	}
	// [sizhuo] fail to find a line, error
	MSG("ERROR: upReqFindLine fail to find the line %lx", lineAddr);
	int i = 0;
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++, i++) {
		MSG("tags[%lx][%d]: %p, state %d, lineAddr %lx, upReq %p, downReq %p", index, i, (*iter), (*iter)->state, (*iter)->lineAddr, (*iter)->upReq, (*iter)->downReq);
		if((*iter)->upReq) {
			(*iter)->upReq->dumpAlways();
		}
		if((*iter)->downReq) {
			(*iter)->downReq->dumpAlways();
		}
	}	
	return 0;
}

CacheLine* LRUCacheArray::downRespFindLine(AddrType lineAddr) {
	const AddrType index = getIndex(lineAddr);

	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->state != CacheLine::I) {
			return (*iter);
		}
	}
	// [sizhuo] fail to find a line, error
	MSG("ERROR: downRespFindLine fail to find the line %lx", lineAddr);
	int i = 0;
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++, i++) {
		MSG("tags[%lx][%d]: %p, state %d, lineAddr %lx, upReq %p, downReq %p", index, i, (*iter), (*iter)->state, (*iter)->lineAddr, (*iter)->upReq, (*iter)->downReq);
		if((*iter)->upReq) {
			(*iter)->upReq->dumpAlways();
		}
		if((*iter)->downReq) {
			(*iter)->downReq->dumpAlways();
		}
	}	
	return 0;
}
