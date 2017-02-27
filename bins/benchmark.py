#!/usr/bin/python

import os
import shutil
import re

# exe & input dir (PARSEC 3.0 & launcher)
bench_root_dir = '/home/szzhang/local-proj/mem-model/simulation/program'

'''
# params to run launcher
launch_param = {}

launch_param['canneal'] = {
        'dev'    : '__THREAD_NUM__ 100 300 100.nets 2'         ,
        'small'  : '__THREAD_NUM__ 10000 2000 100000.nets 32'  ,
        'medium' : '__THREAD_NUM__ 15000 2000 200000.nets 64'  ,
        'large'  : '__THREAD_NUM__ 15000 2000 400000.nets 128' ,
        'thread' : lambda core : core
        }
        '''

# params to run PARSEC 3.0
parsec3_param = {}

parsec3_param['blackscholes'] = {
        'dev'    : '__THREAD_NUM__ in_16.txt blackscholes_dev.out'     ,
        'small'  : '__THREAD_NUM__ in_4K.txt blackscholes_small.out'   ,
        'medium' : '__THREAD_NUM__ in_16K.txt blackscholes_medium.out' ,
        'large'  : '__THREAD_NUM__ in_64K.txt blackscholes_large.out'  ,
        'thread' : lambda core : core
        }

parsec3_param['bodytrack'] = { # we use posix thread model (2)
        'dev'    : 'sequenceB_1 4 1 100 3 2 __THREAD_NUM__'  ,
        'small'  : 'sequenceB_1 4 1 1000 5 2 __THREAD_NUM__' ,
        'medium' : 'sequenceB_2 4 2 2000 5 2 __THREAD_NUM__' ,
        'large'  : 'sequenceB_4 4 4 4000 5 2 __THREAD_NUM__' ,
        'thread' : lambda core : core - (core - 4) / 4 - 1
        }

parsec3_param['facesim'] = { # all input sizes are the same
        'dev'    : '-timing -threads __THREAD_NUM__' ,
        'small'  : '-timing -threads __THREAD_NUM__' ,
        'medium' : '-timing -threads __THREAD_NUM__' ,
        'large'  : '-timing -threads __THREAD_NUM__' ,
        'thread' : lambda core : core # exp shows that we can run 8 threads on 8 cores for simlarge input
        }

parsec3_param['ferret'] = {
        'dev'    : 'corel lsh queries 5 5 __THREAD_NUM__ ferret_dev.out'      ,
        'small'  : 'corel lsh queries 10 20 __THREAD_NUM__ ferret_small.out'  ,
        'medium' : 'corel lsh queries 10 20 __THREAD_NUM__ ferret_medium.out' ,
        'large'  : 'corel lsh queries 10 20 __THREAD_NUM__ ferret_large.out'  ,
        'thread' : lambda core : 1 # exp shows that with 8 cores, running 2 threads will end up with insufficient cores
        # but 1 thread only uses 7 cores
        }

parsec3_param['fluidanimate'] = {
        'dev'    : '__THREAD_NUM__ 3 in_15K.fluid fluidanimate_dev.out'     ,
        'small'  : '__THREAD_NUM__ 5 in_35K.fluid fluidanimate_small.out'   ,
        'medium' : '__THREAD_NUM__ 5 in_100K.fluid fluidanimate_medium.out' ,
        'large'  : '__THREAD_NUM__ 5 in_300K.fluid fluidanimate_large.out'  ,
        'thread' : lambda core : core
        }

parsec3_param['freqmine'] = { # freqmine.out will be removed in program
        'dev'    : 'T10I4D100K_1k.dat 3 freqmine.out __THREAD_NUM__'  ,
        'small'  : 'kosarak_250k.dat 220 freqmine.out __THREAD_NUM__' ,
        'medium' : 'kosarak_500k.dat 410 freqmine.out __THREAD_NUM__' ,
        'large'  : 'kosarak_990k.dat 790 freqmine.out __THREAD_NUM__' ,
        'thread' : lambda core : core # only 4 cores are really used on 8 cores
        }

parsec3_param['streamcluster'] = {
        'dev'    : '3 10 3 16 16 10 none streamcluster_dev.out __THREAD_NUM__'              ,
        'small'  : '10 20 32 4096 4096 1000 none streamcluster_small.out __THREAD_NUM__'    ,
        'medium' : '10 20 64 8192 8192 1000 none streamcluster_medium.out __THREAD_NUM__'   ,
        'large'  : '10 20 128 16384 16384 1000 none streamcluster_large.out __THREAD_NUM__' ,
        'thread' : lambda core : core
        }

parsec3_param['swaptions'] = {
        'dev'    : '-ns 16 -sm 50 -nt __THREAD_NUM__'    , # change 3 to 16, enable 8 threads
        'small'  : '-ns 16 -sm 5000 -nt __THREAD_NUM__'  ,
        'medium' : '-ns 32 -sm 10000 -nt __THREAD_NUM__' ,
        'large'  : '-ns 64 -sm 20000 -nt __THREAD_NUM__' ,
        'thread' : lambda core : core
        }

parsec3_param['vips'] = { # additional arg for thread num
        'dev'    : 'im_benchmark barbados_256x288.v vips_dev.v __THREAD_NUM__'        ,
        'small'  : 'im_benchmark pomegranate_1600x1200.v vips_small.v __THREAD_NUM__' ,
        'medium' : 'im_benchmark vulture_2336x2336.v vips_medium.v __THREAD_NUM__'    ,
        'large'  : 'im_benchmark bigben_2662x5500.v vips_large.v __THREAD_NUM__'      ,
        'thread' : lambda core : core - 2
        }

parsec3_param['x264'] = {
        'dev'    : ('--quiet --qp 20 --partitions b8x8,i4x4 --ref 5 --direct auto --b-pyramid ' +
                    '--weightb --mixed-refs --no-fast-pskip --me umh --subme 7 --analyse b8x8,i4x4 ' +
                    '--threads __THREAD_NUM__ -o x264_dev4.264 eledream_64x36_3.y4m')      ,
        'small'  : ('--quiet --qp 20 --partitions b8x8,i4x4 --ref 5 --direct auto --b-pyramid ' +
                    '--weightb --mixed-refs --no-fast-pskip --me umh --subme 7 --analyse b8x8,i4x4 ' +
                    '--threads __THREAD_NUM__ -o x264_small.264 eledream_640x360_8.y4m')   ,
        'medium' : ('--quiet --qp 20 --partitions b8x8,i4x4 --ref 5 --direct auto --b-pyramid ' +
                    '--weightb --mixed-refs --no-fast-pskip --me umh --subme 7 --analyse b8x8,i4x4 ' +
                    '--threads __THREAD_NUM__ -o x264_medium.264 eledream_640x360_32.y4m') ,
        'large'  : ('--quiet --qp 20 --partitions b8x8,i4x4 --ref 5 --direct auto --b-pyramid ' +
                    '--weightb --mixed-refs --no-fast-pskip --me umh --subme 7 --analyse b8x8,i4x4 ' +
                    '--threads __THREAD_NUM__ -o x264_large.264 eledream_640x360_128.y4m') ,
        'thread' : lambda core : core - 1
        }

# parameter to run SPLASH2x
splash_param = {}

splash_param['barnes'] = {
        'dev'      : '-f input.template' ,
        'small'    : '-f input.template' ,
        'medium'   : '-f input.template' ,
        'large'    : '-f input.template' ,
        'thread'   : lambda core : core ,
        'template' : True
        }

splash_param['fmm'] = {
        'dev'      : '-f input.template' ,
        'small'    : '-f input.template' ,
        'medium'   : '-f input.template' ,
        'large'    : '-f input.template' ,
        'thread'   : lambda core : core ,
        'template' : True
        }

splash_param['ocean_cp'] = {
        'dev'      : '-n258 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800'  ,
        'small'    : '-n514 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800'  ,
        'medium'   : '-n1026 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800' ,
        'large'    : '-n2050 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['ocean_ncp'] = {
        'dev'      : '-n258 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800'  ,
        'small'    : '-n514 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800'  ,
        'medium'   : '-n1026 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800' ,
        'large'    : '-n2050 -p__THREAD_NUM__ -e1e-07 -r20000 -t28800' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['radiosity'] = {
        'dev'      : '-bf 1.5e-1 -batch -room -p __THREAD_NUM__' ,
        'small'    : '-bf 1.5e-1 -batch -room -p __THREAD_NUM__' ,
        'medium'   : '-bf 1.5e-2 -batch -room -p __THREAD_NUM__' ,
        'large'    : '-bf 1.5e-3 -batch -room -p __THREAD_NUM__' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['raytrace'] = {
        'dev'      : '-s -p__THREAD_NUM__ -a4 teapot.env' ,
        'small'    : '-s -p__THREAD_NUM__ -a8 teapot.env' ,
        'medium'   : '-s -p__THREAD_NUM__ -a2 balls4.env' ,
        'large'    : '-s -p__THREAD_NUM__ -a8 balls4.env' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['volrend'] = {
        'dev'      : '__THREAD_NUM__ head-scaleddown4 4'   ,
        'small'    : '__THREAD_NUM__ head-scaleddown4 20'  ,
        'medium'   : '__THREAD_NUM__ head-scaleddown2 50'  ,
        'large'    : '__THREAD_NUM__ head-scaleddown2 100' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['water_nsquared'] = {
        'dev'      : 'input.template' ,
        'small'    : 'input.template' ,
        'medium'   : 'input.template' ,
        'large'    : 'input.template' ,
        'thread'   : lambda core : core ,
        'template' : True
        }

splash_param['water_spatial'] = {
        'dev'      : 'input.template' ,
        'small'    : 'input.template' ,
        'medium'   : 'input.template' ,
        'large'    : 'input.template' ,
        'thread'   : lambda core : core ,
        'template' : True
        }

splash_param['cholesky'] = {
        'dev'      : '-p__THREAD_NUM__ tk14.O' ,
        'small'    : '-p__THREAD_NUM__ tk29.O' ,
        'medium'   : '-p__THREAD_NUM__ tk29.O' ,
        'large'    : '-p__THREAD_NUM__ tk29.O' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['fft'] = {
        'dev'      : '-m18 -p__THREAD_NUM__' ,
        'small'    : '-m20 -p__THREAD_NUM__' ,
        'medium'   : '-m22 -p__THREAD_NUM__' ,
        'large'    : '-m24 -p__THREAD_NUM__' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['lu_cb'] = {
        'dev'      : '-p__THREAD_NUM__ -n512 -b16'  ,
        'small'    : '-p__THREAD_NUM__ -n512 -b16'  ,
        'medium'   : '-p__THREAD_NUM__ -n1024 -b16' ,
        'large'    : '-p__THREAD_NUM__ -n2048 -b16' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['lu_ncb'] = {
        'dev'      : '-p__THREAD_NUM__ -n512 -b16'  ,
        'small'    : '-p__THREAD_NUM__ -n512 -b16'  ,
        'medium'   : '-p__THREAD_NUM__ -n1024 -b16' ,
        'large'    : '-p__THREAD_NUM__ -n2048 -b16' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

splash_param['radix'] = {
        'dev'      : '-p__THREAD_NUM__ -r4096 -n262144 -m524288'       ,
        'small'    : '-p__THREAD_NUM__ -r4096 -n4194304 -m2147483647'  ,
        'medium'   : '-p__THREAD_NUM__ -r4096 -n16777216 -m2147483647' ,
        'large'    : '-p__THREAD_NUM__ -r4096 -n67108864 -m2147483647' ,
        'thread'   : lambda core : core ,
        'template' : False
        }

# helper to remove files in current dir
def remove_here(file_list):
    for f in file_list:
        if os.path.isdir(f):
            shutil.rmtree(f)
        elif os.path.isfile(f):
            os.remove(f)

# helper to copy all files in 1 dir to current dir, return list of copied files
def copy_here(src_dir):
    file_list = []
    if os.path.isdir(src_dir): # only copy when src_dir exists
        file_list = os.listdir(src_dir)
        # remove existing files in current dir
        remove_here(file_list)
        # copy here
        for f in file_list:
            src_path = os.path.join(src_dir, f)
            dst_path = os.path.join('.', f)
            if os.path.isdir(src_path):
                shutil.copytree(src_path, dst_path)
            else:
                shutil.copy(src_path,dst_path)
    return file_list

'''
# subroutine to run one benchmark in launcher
def run_launcher(name, size, mem_model, core_num, thread_num = 0):
    # PARSEC real thread num = input thread num + 1 (main thread)
    # But SPLASH real thread num = input thread num
    # so we need core_num (# of CPU) & thread_num (input to benchmark)

    # check we have param for benchmark
    if name not in launch_param:
        print(name + " doesn't have parameters!")
        return False
    elif size not in launch_param[name]:
        print(name + " doesn't have parameter for size " + size)
        return False

    # copy input files
    input_dir = os.path.join(bench_root_dir, 'launcher', name, size)
    all_inputs = copy_here(input_dir)

    # change esesc.conf
    if thread_num == 0: # thread_num is at default, we use func in launch_param to calculate it
        thread_num = launch_param[name]['thread'](core_num)
    cpu_max_id = str(core_num - 1)
    bench_cmd = 'launcher -- ' + name + ' ' + launch_param[name][size]
    bench_cmd = re.sub('__THREAD_NUM__', str(thread_num), bench_cmd)
    report_file = 'parsec_' + name + '_' + size + '_' + mem_model + '_c' + str(core_num) + '_t' + str(thread_num)

    shell_cmd = (
            "sed 's/__CPU_MAX_ID__/" + cpu_max_id + "/g' esesc.conf.template | " + 
            "sed 's/__MEMORY_MODEL__/" + mem_model + "/g' | " +
            "sed 's/__BENCH_NAME__/" + bench_cmd + "/g' | " +
            "sed 's/__REPORT_FILE__/" + report_file + "/g' > esesc.conf"
            )
    print(shell_cmd)
    os.system(shell_cmd)

    # special cases
    if name == 'facesim':
        if os.path.isdir('Storytelling'):
            shutil.rmtree('Storytelling')
        os.makedirs('Storytelling/output')

    # run
    log_file = report_file + '.log'
    shell_cmd = '../main/esesc 2>&1 | tee ' + log_file
    print(shell_cmd)
    os.system(shell_cmd)

    # remove input files
    remove_here(all_inputs)

    # change report file mode
    shell_cmd = 'chmod 644 esesc_' + report_file + '.*'
    os.system(shell_cmd)

    return True
####
'''

# subroutine to run PARSEC 3.0 benchmarks
def run_parsec3(name, size, mem_model, core_num, prefetch, thread_num = 0):
    # check we have param for benchmark
    if name not in parsec3_param:
        print(name + " doesn't have parameters!")
        return False
    elif size not in parsec3_param[name]:
        print(name + " doesn't have parameter for size " + size)
        return False

    # special mem model: usqtso -- unlimit SQ
    unlimitSQ = mem_model == 'usqtso'

    # copy exe
    if os.path.isfile(name): # remove existing one
        os.remove(name)
    exe_path = os.path.join(bench_root_dir, 'parsec', name, name)
    if not os.path.isfile(exe_path):
        print(exe_path + " doesn't exist!")
        return False
    shutil.copy(exe_path, '.')

    # copy input files
    if name == 'facesim': # facesim all inputs are the same, only in dev folder
        input_dir = os.path.join(bench_root_dir, 'parsec', name, 'dev')
    else:
        input_dir = os.path.join(bench_root_dir, 'parsec', name, size)
    all_inputs = copy_here(input_dir)
    
    # change esesc.conf
    if thread_num == 0: # thread_num is at default, we use func in parsec_param to calculate it
        thread_num = parsec3_param[name]['thread'](core_num)
    cpu_max_id = str(core_num - 1)
    bench_cmd = name + ' ' + parsec3_param[name][size]
    bench_cmd = re.sub('__THREAD_NUM__', str(thread_num), bench_cmd)
    report_file = 'parsec_' + name + '_' + mem_model + '_' + size + '_c' + str(core_num) + '_t' + str(thread_num) + '_pf' + prefetch

    shell_cmd = (
            "sed 's/__CPU_MAX_ID__/" + cpu_max_id + "/g' esesc.conf.template | " + 
            "sed 's/__MEMORY_MODEL__/" + ('tso' if mem_model == 'usqtso' else mem_model) + "/g' | " + # simulator cannot recognize usqtso
            "sed 's/__STORE_PREFETCH__/" + prefetch + "/g' | "
            "sed 's/__BENCH_NAME__/" + bench_cmd + "/g' | " +
            "sed 's/__REPORT_FILE__/" + report_file + "/g' > esesc.conf"
            )
    print(shell_cmd)
    os.system(shell_cmd)

    # for unlimit SQ, change simu.conf: SQ size to 1024
    if unlimitSQ:
        os.system("sed -i 's/^maxStores.*$/maxStores = 1024/g' simu.conf")

    # special addtional work
    if name == 'facesim': # make output dir for facesim
        if os.path.isdir('Storytelling'):
            shutil.rmtree('Storytelling')
        os.makedirs('Storytelling/output')
    # run
    log_file = report_file + '.log'
    shell_cmd = '../main/esesc 2>&1 | tee ' + log_file
    print(shell_cmd)
    os.system(shell_cmd)

    # special work to save result for bodytrack
    if name == 'bodytrack':
        shell_cmd = (
                'mv ' +
                parsec3_param[name][size].split()[0] +
                '/*.bmp .'
                )
        print(shell_cmd)
        os.system(shell_cmd)

    # remove input files & exe
    #remove_here(all_inputs)
    #os.remove(name)

    # change report file mode
    shell_cmd = 'chmod 644 esesc_' + report_file + '.*'
    os.system(shell_cmd)

    return True
####

# subroutine to runi SPLASH2X benchmarks
def run_splash(name, size, mem_model, core_num, prefetch, thread_num = 0):
    # check we have param for benchmark
    if name not in splash_param:
        print(name + " doesn't have parameters!")
        return False
    elif size not in splash_param[name]:
        print(name + " doesn't have parameter for size " + size)
        return False

    # special mem model: usqtso -- unlimit SQ
    unlimitSQ = mem_model == 'usqtso'

    # copy exe
    if (mem_model in ['rcm', 'wmm']) and (name in ['barnes', 'fmm']):
        exe_name = name + '_wmm'
        exe_path = os.path.join(bench_root_dir, 'splash_wmm', exe_name)
    else:
        exe_name = name
        exe_path = os.path.join(bench_root_dir, 'splash', name, name)
    if os.path.isfile(exe_name): # remove existing one
        os.remove(exe_name)
    if not os.path.isfile(exe_path):
        print(exe_path + " doesn't exist!")
        return False
    shutil.copy(exe_path, '.')

    # copy input files
    input_dir = os.path.join(bench_root_dir, 'splash', name, size)
    all_inputs = copy_here(input_dir)

    # change esesc.conf
    if thread_num == 0: # thread_num is at default, we use func in splash_param to calculate it
        thread_num = splash_param[name]['thread'](core_num)
    cpu_max_id = str(core_num - 1)
    bench_cmd = exe_name + ' ' + splash_param[name][size]
    bench_cmd = re.sub('__THREAD_NUM__', str(thread_num), bench_cmd)
    report_file = 'splash_' + name + '_' + mem_model + '_' + size + '_c' + str(core_num) + '_t' + str(thread_num) + '_pf' + prefetch 

    shell_cmd = (
            "sed 's/__CPU_MAX_ID__/" + cpu_max_id + "/g' esesc.conf.template | " + 
            "sed 's/__MEMORY_MODEL__/" + ('tso' if mem_model == 'usqtso' else mem_model) + "/g' | " + # simulator cannot recognize usqtso
            "sed 's/__STORE_PREFETCH__/" + prefetch + "/g' | "
            "sed 's/__BENCH_NAME__/" + bench_cmd + "/g' | " +
            "sed 's/__REPORT_FILE__/" + report_file + "/g' > esesc.conf"
            )
    print(shell_cmd)
    os.system(shell_cmd)

    # for unlimit SQ, change simu.conf: SQ size to 1024
    if unlimitSQ:
        os.system("sed -i 's/^maxStores.*$/maxStores = 1024/g' simu.conf")

    # some benchmarks needs to change input.template
    shell_cmd = "sed -i 's/NUMPROCS/" + str(thread_num) + "/g' input.template"
    os.system(shell_cmd)
    
    # run
    log_file = report_file + '.log'
    shell_cmd = '../main/esesc 2>&1 | tee ' + log_file
    print(shell_cmd)
    os.system(shell_cmd)

    # remove input files & exe
    #remove_here(all_inputs)
    #os.remove(exe_name)

    # change report file mode
    shell_cmd = 'chmod 644 esesc_' + report_file + '.*'
    os.system(shell_cmd)

    return True
####

# subroutine to run microbenchmarks
def run_micro(name, iteration, mem_model, prefetch, verify_ld = False):
    # special mem model: usqtso -- unlimit SQ
    unlimitSQ = mem_model == 'usqtso'

    # copy exe
    if os.path.isfile(name): # remove existing one
        os.remove(name)
    exe_path = os.path.join(bench_root_dir, 'micro', name, name)
    if not os.path.isfile(exe_path):
        print(exe_path + " doesn't exist!")
        return False
    shutil.copy(exe_path, '.')

    # change esesc.conf
    bench_cmd = '{} {}'.format(name, iteration)
    report_file = 'micro_{}_{}_pf{}_v{}'.format(name, mem_model, prefetch, int(verify_ld))

    shell_cmd = (
            "sed 's/__CPU_MAX_ID__/0/g' esesc.conf.template | " +  # 1 core
            "sed 's/__MEMORY_MODEL__/" + ('tso' if mem_model == 'usqtso' else mem_model) + "/g' | " + # simulator cannot recognize usqtso
            "sed 's/__STORE_PREFETCH__/" + prefetch + "/g' | "
            "sed 's/__BENCH_NAME__/" + bench_cmd + "/g' | " +
            "sed 's/__REPORT_FILE__/" + report_file + "/g' > esesc.conf"
            )
    print(shell_cmd)
    os.system(shell_cmd)

    # for unlimit SQ, change simu.conf: SQ size to 1024
    if unlimitSQ:
        os.system("sed -i 's/^maxStores.*$/maxStores = 1024/g' simu.conf")

    # for load verification, change simu.conf
    if verify_ld:
        os.system("sed -i 's/^sctsoVerifyLd.*$/sctsoVerifyLd = true/g' simu.conf")

    # run
    log_file = report_file + '.log'
    shell_cmd = '../main/esesc 2>&1 | tee ' + log_file
    print(shell_cmd)
    os.system(shell_cmd)

    # remove input files & exe
    #remove_here(all_inputs)
    #os.remove(name)

    # change report file mode
    shell_cmd = 'chmod 644 esesc_' + report_file + '.*'
    os.system(shell_cmd)

    return True
###
