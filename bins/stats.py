#!/usr/bin/python
import re
import scipy.io as sio
import sys
import os
import json

# check arg
if len(sys.argv) < 2:
	print 'Usage: ./stats.py <esesc result file> [output folder]'
	sys.exit()

if not os.path.isfile(sys.argv[1]):
	print 'ERROR: cannot find {}'.format(sys.argv[1])

# read file
fp = open(sys.argv[1], 'r')
resultFile = list(fp)
fp.close()

# data to save
saveData = {}

# get basic param
coreNum = -1
maxLd = -1
maxSt = -1
robSize = -1
retireWidth = -1
issueWidth = -1

for line in resultFile:
	if coreNum == -1:
		m = re.search(r'cpusimu\[0:(\d+)\]', line)
		if m:
			coreNum = int(m.group(1)) + 1
			continue
	if maxLd == -1:
		m = re.search(r'maxLoads\s*=\s*(\d+)', line)
		if m:
			maxLd = int(m.group(1))
			continue
	if maxSt == -1:
		m = re.search(r'maxStores\s*=\s*(\d+)', line)
		if m:
			maxSt = int(m.group(1))
			continue
	if robSize == -1:
		m = re.search(r'robSize\s*=\s*(\d+)', line)
		if m:
			robSize = int(m.group(1))
	if retireWidth == -1:
		m = re.search(r'retireWidth\s*=\s*(\d+)', line)
		if m:
			retireWidth = int(m.group(1))
			continue
	if issueWidth == -1:
		m = re.search(r'issueWidth\s*=\s*(\d+)', line)
		if m:
			issueWidth = int(m.group(1))
			continue
	if coreNum > 0 and maxLd > 0 and maxSt > 0 and robSize > 0 and retireWidth > 0 and issueWidth > 0:
		break;

saveData['coreNum'] = coreNum
saveData['maxLd'] = maxLd
saveData['maxSt'] = maxSt
saveData['robSize'] = robSize
saveData['retireWidth'] = retireWidth
saveData['issueWidth'] = issueWidth

print ''

# overall performance
wallClock = -1
simTime = -1
inst = [-1] * coreNum
cycle = [-1] * coreNum

for line in resultFile:
	if wallClock == -1:
		m = re.search(r'OS:wallClock\s*=\s*(\d+)\.0+', line)
		if m:
			wallClock = int(m.group(1))
			continue
	if simTime == -1:
		m = re.search(r'OS:simTime\s*=\s*(\d+)\.0+', line)
		if m:
			simTime = int(m.group(1))
			continue
	m = re.search(r'P\((\d+)\):nCommitted\s*=\s*(\d+)\.0+', line)
	if m:
		inst[int(m.group(1))] = int(m.group(2))
		continue
	m = re.search(r'P\((\d+)\)_activeCyc\s*=\s*(\d+)\.0+', line)
	if m:
		cycle[int(m.group(1))] = int(m.group(2))
		continue
	if (reduce(lambda acc, x: (acc and x > -1), inst, True) and
			reduce(lambda acc, x: (acc and x > -1), cycle, True) and
			wallClock > -1 and simTime > -1):
		break

saveData['wallClock'] = wallClock
saveData['simTime'] = simTime
saveData['inst'] = inst
saveData['cycle'] = cycle

print '-- Overall Peformanace'
print 'wallClock = {}, simTime = {}'.format(wallClock, simTime)
print '{:<6s}{:<14s}{:<14s}{:<14s}'.format(' ', 'cycle', 'inst', 'IPC')
for i in range(0, coreNum):
	print '{:<6s}{:<14d}{:<14d}{:<14f}'.format('P({})'.format(i),
			cycle[i], inst[i], float(inst[i]) / float(cycle[i]))
print ''

# inst distribution
nRALU = [0] * coreNum
nAALU = [0] * coreNum
nBALU = [0] * coreNum
nLALU_LD = [0] * coreNum
nLALU_REC = [0] * coreNum
nSALU_ST = [0] * coreNum
nSALU_ADDR = [0] * coreNum
nSALU_COM = [0] * coreNum
nCALU_3C = [0] * coreNum
nCALU_5C = [0] * coreNum
nCALU_7C = [0] * coreNum
nMALU = [0] * coreNum

for line in resultFile:
	m = re.search(r'P\((\d+)\)_ExeEngine_i(\w+):n=(\d+)\.0+', line)
	if m:
		coreId = int(m.group(1))
		instType = m.group(2)
		count = int(m.group(3))
		if instType == 'RALU':
			nRALU[coreId] = count
		elif instType == 'AALU':
			nAALU[coreId] = count
		elif re.search(r'^BALU', instType):
			nBALU[coreId] += count
		elif instType == 'LALU_LD':
			nLALU_LD[coreId] = count
		elif instType == 'LALU_REC':
			nLALU_REC[coreId] = count
		elif instType == 'SALU_ST':
			nSALU_ST[coreId] = count
		elif instType == 'SALU_ADDR':
			nSALU_ADDR[coreId] = count
		elif instType == 'SALU_COM':
			nSALU_COM[coreId] = count
		elif instType == 'CALU_FPALU' or instType == 'CALU_MULT':
			nCALU_3C[coreId] += count
		elif instType == 'CALU_FPMULT':
			nCALU_5C[coreId] += count
		elif instType == 'CALU_FPDIV' or instType == 'CALU_DIV':
			nCALU_7C[coreId] += count
		elif re.search(r'^MALU', instType):
			nMALU[coreId] += count
		else:
			if count > 0:
				print 'ERROR: unknow inst type {}'.format(instType)

print '-- Instruction Distribution'
print '{:<6s}{:<6s}{:<6s}{:<6s}{:<9s}{:<10s}{:<9s}{:<11s}{:<10s}{:<9s}{:<9s}{:<9s}{:<6s}'.format('(%)', 
'RALU', 'AALU', 'BALU', 'LALU_LD', 'LALU_REC', 'SALU_ST', 'SALU_ADDR', 'SALU_COM', 'CALU_3C', 'CALU_5C', 'CALU_7C', 'MALU')
for i in range(0, coreNum):
	if inst[i] <= 0:
		print '{:<6s}{:<6s}{:<6s}{:<6s}{:<9s}{:<10s}{:<9s}{:<11s}{:<10s}{:<9s}{:<9s}{:<9s}{:<6s}'.format('P({})'.format(i),
				'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan')
	else:
		print '{:<6s}{:<6.2f}{:<6.1f}{:<6.1f}{:<9.1f}{:<10.3f}{:<9.1f}{:<11.1f}{:<10.3f}{:<9.1f}{:<9.1f}{:<9.1f}{:<6.1f}'.format('P({})'.format(i),
				float(nRALU[i]) / float(inst[i]) * 100.0, float(nAALU[i]) / float(inst[i]) * 100.0, float(nBALU[i]) / float(inst[i]) * 100.0,
				float(nLALU_LD[i]) / float(inst[i]) * 100.0, float(nLALU_REC[i]) / float(inst[i]) * 100.0,
				float(nSALU_ST[i]) / float(inst[i]) * 100.0, float(nSALU_ADDR[i]) / float(inst[i]) * 100.0,
				float(nSALU_COM[i]) / float(inst[i]) * 100.0, float(nCALU_3C[i]) / float(inst[i]) * 100.0, 
				float(nCALU_5C[i]) / float(inst[i]) * 100.0, float(nCALU_7C[i]) / float(inst[i]) * 100.0, float(nMALU[i]) / float(inst[i]) * 100.0)
print ''

# unaligned memory access
unalignLd = [0] * coreNum
unalignSt = [0] * coreNum

for line in resultFile:
	m = re.search(r'P\((\d+)\)_MTLSQ_nUnalign(Ld|St)\s*=\s*(\d+)\.0+', line)
	if m:
		coreId = int(m.group(1))
		count = int(m.group(3))
		if m.group(2) == 'Ld':
			unalignLd[coreId] = count
		elif m.group(2) == 'St':
			unalignSt[coreId] = count
		else:
			print 'ERROR: unknow unalign type {}'.format(m.group(2))

print '-- Unaligned Memory Access'
print '{:<6s}{:<14s}{:<14s}'.format('', 'Unalign Ld%', 'Unalign St%')
for i in range(0, coreNum):
	print '{:<6s}{:<14s}{:<14s}'.format('P({})'.format(i),
			'nan' if nLALU_LD[i] <= 0 else '{:<14.3f}'.format(float(unalignLd[i]) / float(nLALU_LD[i]) * 100.0),
			'nan' if nSALU_ST[i] <= 0 else '{:<14.3f}'.format(float(unalignSt[i]) / float(nSALU_ST[i]) * 100.0))
print ''

# retire port stall
retireStallByExLd = [0] * coreNum
retireStallByExOther = [0] * coreNum
retireStallByFlushInv = [0] * coreNum
retireStallByFlushSt = [0] * coreNum
retireStallByFlushLd = [0] * coreNum
retireStallByComSQ = [0] * coreNum
retireStallByEmpty = [0] * coreNum

for line in resultFile:
	m = re.search(r'P\((\d+)\)_retireStallBy(\w+)\s*=\s*(\d+)\.0+', line)
	if m:
		coreId = int(m.group(1))
		stallType = m.group(2)
		count = int(m.group(3))
		if stallType == 'Empty':
			retireStallByEmpty[coreId] = count
		elif stallType == 'ComSQ':
			retireStallByComSQ[coreId] = count
		elif stallType == 'Flush_CacheInv':
			retireStallByFlushInv[coreId] = count
		elif stallType == 'Flush_Load':
			retireStallByFlushLd[coreId] = count
		elif stallType == 'Flush_Store':
			retireStallByFlushSt[coreId] = count
		elif stallType == 'Ex_iLALU_LD':
			retireStallByExLd[coreId] = count
		elif re.match(r'Ex_i', stallType):
			retireStallByExOther[coreId] += count
		else:
			print 'ERROR: unkonw retire stall type {}'.format(stallType)

saveData['retireStallByExLd'] = retireStallByExLd
saveData['retireStallByExOther'] = retireStallByExOther
saveData['retireStallByFlushInv'] = retireStallByFlushInv
saveData['retireStallByFlushSt'] = retireStallByFlushSt
saveData['retireStallByFlushLd'] = retireStallByFlushLd
saveData['retireStallByComSQ'] = retireStallByComSQ
saveData['retireStallByEmpty'] = retireStallByEmpty

print '-- Retire Port BW Distribution'
print '{:<6s}LdEx   OtherEx  FlushSt  FlushLd  FlushInv  ComSQ  Empty  Active'.format('(%)')
for i in range(0, coreNum):
	if cycle[i] <= 0:
		print '{:<6s}{:<7s}{:<9s}{:<9s}{:<9s}{:<10s}{:<7s}{:<7s}{:<8s}'.format('P({})'.format(i),
				'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan')
	else:
		print '{:<6s}{:<7.2f}{:<9.2f}{:<9.3f}{:<9.3f}{:<10.3f}{:<7.2f}{:<7.2f}{:<8.2f}'.format('P({})'.format(i),
				float(retireStallByExLd[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByExOther[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByFlushSt[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByFlushLd[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByFlushInv[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByComSQ[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				float(retireStallByEmpty[i]) / float(retireWidth) / float(cycle[i]) * 100.0,
				100.0 - float(retireStallByExLd[i] + retireStallByExOther[i] + retireStallByFlushLd[i] 
					+ retireStallByFlushInv[i] + retireStallByFlushSt[i] + retireStallByComSQ[i]
					+ retireStallByEmpty[i]) / float(retireWidth) / float(cycle[i]) * 100.0)
print ''

# issue port stall
issueOutBr = [0] * coreNum
issueOutLd = [0] * coreNum
issueOutSt = [0] * coreNum
issueSmallReg = [0] * coreNum
issueSmallWin = [0] * coreNum
issueSmallROB = [0] * coreNum
issueReplay = [0] * coreNum
issueSyscall = [0] * coreNum

for line in resultFile:
	m = re.search(r'P\((\d+)\)_n(\w+)Stall\s*=\s*(\d+)\.0+', line)
	if m:
		coreId = int(m.group(1))
		stallType = m.group(2)
		count = int(m.group(3))
		if stallType == 'OutsBranches':
			issueOutBr[coreId] = count
		elif stallType == 'OutsLoads':
			issueOutLd[coreId] = count
		elif stallType == 'OutsStores':
			issueOutSt[coreId] = count
		elif stallType == 'SmallREG':
			issueSmallReg[coreId] = count
		elif stallType == 'SmallROB':
			issueSmallROB[coreId] = count
		elif stallType == 'SmallWin':
			issueSmallWin[coreId] = count
		elif stallType == 'Syscall':
			issueSyscall[coreId] = count
		elif stallType == 'Replays':
			issueReplay[coreId] = count
		else:
			print 'ERROR: unknown issue stall type {}'.format(stallType)

print '-- Issue Port Stall Ratio (Legacy & Inaccurate)'
print '{:<6s}OutBr  OutLd  OutSt  SmallReg  SmallWin  SmallROB  Replay  Syscall'.format('(%)')
for i in range(0, coreNum):
	if cycle[i] <= 0:
		print '{:<6s}{:<7s}{:<7s}{:<7s}{:<10s}{:<10s}{:<10s}{:<8s}{:<9s}'.format('P({})'.format(i),
				'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan', 'nan')
	else:
		print '{:<6s}{:<7.2f}{:<7.2f}{:<7.2f}{:<10.2f}{:<10.2f}{:<10.2f}{:<8.2f}{:<9.2f}'.format('P({})'.format(i),
				float(issueOutBr[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueOutLd[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueOutSt[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueSmallReg[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueSmallWin[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueSmallROB[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueReplay[i]) / float(issueWidth) / float(cycle[i]) * 100.0,
				float(issueSyscall[i]) / float(issueWidth) / float(cycle[i]) * 100.0)
print ''

# rob usage
robUsage = [[0] * (robSize + 1)] * coreNum

for line in resultFile:
	m = re.search(r'P\((\d+)\)_robUsage\((\d+)\)\s*=\s*(\d+)\.0+', line)
	if m:
		robUsage[int(m.group(1))][int(m.group(2))] = int(m.group(3))

print '-- ROB Usage'
print '{:<6s}[0.0, 0.2)  [0.2, 0.4)  [0.4, 0.6)  [0.6, 0.8)  [0.8, 1.0]'.format('(%)')
for i in range(0, coreNum):
	if cycle[i] <= 0:
		print '{:<6s}{:<12s}{:<12s}{:<12s}{:<12s}{:<12s}'.format('P({})'.format(i),
				'nan', 'nan', 'nan', 'nan', 'nan')
	else:
		print '{:<6s}{:<12.2f}{:<12.2f}{:<12.2f}{:<12.2f}{:<12.2f}'.format('P({})'.format(i),
				float(sum(robUsage[i][0 : int(0.2 * robSize)])) / float(cycle[i]) * 100.0,
				float(sum(robUsage[i][int(0.2 * robSize) : int(0.4 * robSize)])) / float(cycle[i]) * 100.0,
				float(sum(robUsage[i][int(0.4 * robSize) : int(0.6 * robSize)])) / float(cycle[i]) * 100.0,
				float(sum(robUsage[i][int(0.6 * robSize) : int(0.8 * robSize)])) / float(cycle[i]) * 100.0,
				float(sum(robUsage[i][int(0.8 * robSize) :])) / float(cycle[i]) * 100.0)
print ''

# average memory latency
readMemLat = [0] * coreNum
readMemNum = [0] * coreNum
writeMemLat = [0] * coreNum
writeMemNum = [0] * coreNum
prefetchMemLat = [0] * coreNum
prefetchMemNum = [0] * coreNum

for line in resultFile:
	m = re.search(r'DL1\((\d+)\)_(\w+)MemLat:n=(\d+)::v=(\d+\.\d+)', line)
	if m:
		coreId = int(m.group(1))
		memType = m.group(2)
		num = int(m.group(3))
		lat = float(m.group(4))
		if memType == 'read':
			readMemNum[coreId] = num
			readMemLat[coreId] = lat
		elif memType == 'write':
			writeMemNum[coreId] = num
			writeMemLat[coreId] = lat
		elif memType == 'prefetch':
			prefetchMemNum[coreId] = num
			prefetchMemLat[coreId] = lat

saveData['readMemNum'] = readMemNum
saveData['readMemLat'] = readMemLat
saveData['writeMemNum'] = writeMemNum
saveData['writeMemLat'] = writeMemLat
saveData['prefetchMemNum'] = prefetchMemNum
saveData['prefetchMemLat'] = prefetchMemLat

print '-- Memory Access'
print '{:<6s}{:<16s}{:<16s}{:<16s}'.format('', 'Read', 'Write', 'Prefetch')
print ('{:<6s}' + 'ReqPKI  AvgLat  ' * 3).format('')
for i in range(0, coreNum):
	print ('{:<6s}' + '{:<8s}{:<8s}' * 3).format('P({})'.format(i),
			'nan' if inst[i] <= 0 else '{:<8.1f}'.format(float(readMemNum[i]) / float(inst[i]) * 1000.0), '{:<8.1f}'.format(readMemLat[i]),
			'nan' if inst[i] <= 0 else '{:<8.1f}'.format(float(writeMemNum[i]) / float(inst[i]) * 1000.0), '{:<8.1f}'.format(writeMemLat[i]),
			'nan' if inst[i] <= 0 else '{:<8.1f}'.format(float(prefetchMemNum[i]) / float(inst[i]) * 1000.0), '{:<8.1f}'.format(prefetchMemLat[i]))
print ''

# cache miss
# since memory will convert read req to set ex resp,
# the avg lat in MSHR is inaccurate (because it classifies using resp action)
readMissNum = {'DL1': [0] * coreNum, 'L2': [0] * coreNum, 'L3': [0]}
writeMissNum = {'DL1': [0] * coreNum, 'L2': [0] * coreNum, 'L3': [0]}
prefetchMissNum = {'DL1': [0] * coreNum, 'L2': [0] * coreNum, 'L3': [0]}

for line in resultFile:
	m = re.search(r'(DL1|L2|L3)\((\d+)\)_(read|write|prefetch)(Half)?Miss\s*=\s*(\d+)\.0+', line)
	if m:
		cacheType = m.group(1)
		coreId = int(m.group(2))
		missType = m.group(3)
		count = int(m.group(5))
		if missType == 'read':
			readMissNum[cacheType][coreId] = count
		elif missType == 'write':
			writeMissNum[cacheType][coreId] = count
		elif missType == 'prefetch':
			prefetchMissNum[cacheType][coreId] = count
		else:
			print 'ERROR: unknown miss type {}'.format(missType)

print '-- Cache Miss'
print '{:<8s}Read  Write  Prefetch'.format('(MPKI)')
for cache in ['DL1', 'L2', 'L3']:
	for i in range(0, 1 if cache == 'L3' else coreNum):
		instNum = float(sum(inst) if cache == 'L3' else inst[i])
		print '{:8s}{:<6.1f}{:<7.1f}{:<8.1f}'.format('{}({})'.format(cache, i),
				float(readMissNum[cache][i]) / instNum * 1000.0, 
				float(writeMissNum[cache][i]) / instNum * 1000.0, 
				float(prefetchMissNum[cache][i]) / instNum * 1000.0)
print ''


# save data to .mat & .json files
if len(sys.argv) >= 3:
	if not os.path.isdir(sys.argv[2]):
		print 'ERROR: cannot file {}'.format(sys.argv[2])

	fileName = os.path.basename(sys.argv[1]).split('.')[0]
	jsonName = os.path.join(sys.argv[2], fileName + '.json')
	matName = os.path.join(sys.argv[2], fileName + '.mat')
	if os.path.isfile(jsonName) or os.path.isfile(matName):
		print 'ERROR: {} or {} already exists'.format(jsonName, matName)

	sio.savemat(matName, mdict = saveData, appendmat = False, oned_as = 'column')

	fp = open(jsonName, 'w')
	json.dump(saveData, fp)
	fp.close()
