#!/usr/bin/python

import os
import shutil
import re

# benmark dir
bench_root_dir = '/home/szzhang/local-proj/mem-model/simulation/program'

# params to run benchmarks
param = {}

param['blackscholes'] = {
		'dev'    : '__THREAD_NUM__ in_16.txt prices.txt'  ,
		'small'  : '__THREAD_NUM__ in_4K.txt prices.txt'  ,
		'medium' : '__THREAD_NUM__ in_16K.txt prices.txt' ,
		'large'  : '__THREAD_NUM__ in_64K.txt prices.txt'
		}

param['bodytrack'] = { # we use posix thread
		'dev'    : 'sequenceB_1 4 1 100 3 2 __THREAD_NUM__'  ,
		'small'  : 'sequenceB_1 4 1 1000 5 2 __THREAD_NUM__' ,
		'medium' : 'sequenceB_2 4 2 2000 5 2 __THREAD_NUM__' ,
		'large'  : 'sequenceB_4 4 4 4000 5 2 __THREAD_NUM__' 
		}

param['fluidanimate'] = {
		'dev'    : '__THREAD_NUM__ 3 in_15K.fluid out.fluid'  ,
		'small'  : '__THREAD_NUM__ 5 in_35K.fluid out.fluid'  ,
		'medium' : '__THREAD_NUM__ 5 in_100K.fluid out.fluid' ,
		'large'  : '__THREAD_NUM__ 5 in_300K.fluid out.fluid'
		}

param['freqmine'] = { # need to env OMP_NUM_THREADS
		'dev'    : 'T10I4D100K_1k.dat 3'  ,
		'small'  : 'kosarak_250k.dat 220' ,
		'medium' : 'kosarak_500k.dat 410' ,
		'large'  : 'kosarak_990k.dat 790'
		}

# helper to remove files
def remove_files(file_list):
	for f in file_list:
		if os.path.isdir(f):
			shutil.rmtree(f)
		elif os.path.isfile(f):
			os.remove(f)

# subroutine to run one benchmark
def run_bench(name, size, thread_num):
	# PARSEC real thread num = input thread num + 1 (main thread)
	# in simulator, core 0 is always a dummy core
	# here input thread_num is the input to PARSEC

	# check we have param for benchmark
	if name not in param:
		print(name + " doesn't have parameters!")
		return False
	elif size not in param[name]:
		print(name + " doesn't have parameter for size " + size)
		return False

	# benchmark dir
	bench_exe = os.path.join(bench_root_dir, name, name)
	input_dir = os.path.join(bench_root_dir, name, size)
	# check exe existence
	if not os.path.isfile(bench_exe):
		print(bench_exe + " doesn't exist!")
		return False

	# copy exe
	if os.path.isfile(name):
		os.remove(name) # remove old one
	shutil.copy(bench_exe, '.')
	# copy input files to here
	all_inputs = [] # list of all input files
	if os.path.isdir(input_dir): # input may not exist
		all_inputs = os.listdir(input_dir)
		# remove existing files
		remove_files(all_inputs)
		# copy here
		for f in all_inputs:
			src_path = os.path.join(input_dir, f)
			dst_path = os.path.join('.', f)
			if os.path.isdir(src_path):
				shutil.copytree(src_path, dst_path)
			else:
				shutil.copy(src_path,dst_path)
	
	# change esesc.conf
	cpu_max_id = str(thread_num)
	bench_cmd = (name + ' ' + param[name][size])
	bench_cmd = re.sub('__THREAD_NUM__', str(thread_num), bench_cmd)
	report_file = name + '_' + size + '_th' + str(thread_num)

	shell_cmd = (
			"sed 's/__CPU_MAX_ID__/" + cpu_max_id + "/g' esesc.conf.template | " + 
			"sed 's/__BENCH_NAME__/" + bench_cmd + "/g' | " +
			"sed 's/__REPORT_FILE__/" + report_file + "/g' > esesc.conf"
			)
	print(shell_cmd)
	os.system(shell_cmd)

	# for freqmine, export OMP_NUM_THREADS
	if name == 'freqmine':
		os.environ['OMP_NUM_THREADS'] = thread
	
	# run
	log_file = report_file + '.log'
	shell_cmd = '../main/esesc 2>&1 | tee ' + log_file
	print(shell_cmd)
	os.system(shell_cmd)

	# remove input files & exe
	remove_files(all_inputs)
	os.remove(name)

	# change report file mode
	shell_cmd = 'chmod 644 esesc_' + report_file + '.*'
	os.system(shell_cmd)

	return True
####

run_bench('blackscholes', 'dev', 4)
run_bench('bodytrack', 'dev', 4)
run_bench('freqmine', 'dev', 4)
