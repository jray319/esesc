#include "CacheArray.h"
#include <queue>
#include "nanassert.h"
#include "MemRequest.h"

LRUCacheArray::LRUCacheArray(const uint32_t sz, const uint32_t lineSz, const uint32_t a, const int upNodeNum)
	: CacheArray(sz, lineSz, a)
	, tags(0)
{
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

	// [sizhuo] find hit line
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->state != CacheLine::I) {
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

	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++) {
		if((*iter)->lineAddr == lineAddr && (*iter)->upReq == mreq) {
			return (*iter);
		}
	}
	// [sizhuo] fail to find a line, error
	MSG("ERROR: upReqFindLine fail to find the line %lx", lineAddr);
	int i = 0;
	for(CacheSet::iterator iter = tags[index].begin(); iter != tags[index].end(); iter++, i++) {
		MSG("tags[%lx][%d]: state %d, lineAddr %lx, upReq %p, downReq %p", index, i, (*iter)->state, (*iter)->lineAddr, (*iter)->upReq, (*iter)->downReq);
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
		MSG("tags[%lx][%d]: state %d, lineAddr %lx, upReq %p, downReq %p", index, i, (*iter)->state, (*iter)->lineAddr, (*iter)->upReq, (*iter)->downReq);
	}	
	return 0;
}
