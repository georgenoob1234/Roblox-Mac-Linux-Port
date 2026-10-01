#!/usr/bin/env python3
"""Check the native helper against Linux sibling lists, changing only this process."""
import ctypes, os, pathlib, sys, threading, time

lib=ctypes.CDLL(sys.argv[1],use_errno=True)
count=lib.trackb_check_cpu_count
count.argtypes=[ctypes.c_int];count.restype=ctypes.c_int
original=os.sched_getaffinity(0)
def core(cpu):
    return (pathlib.Path('/sys/devices/system/cpu')/f'cpu{cpu}/topology/thread_siblings_list').read_text().strip()
def check(mask):
    os.sched_setaffinity(0,mask)
    expected=len({core(cpu) for cpu in mask})
    assert count(0)==len(mask) and count(1)==expected, (mask,count(0),count(1),expected)
    return expected
try:
    cpus=sorted(original);groups={}
    for cpu in cpus:groups.setdefault(core(cpu),[]).append(cpu)
    masks=[set(cpus),{cpus[0]},set(cpus[::2]),{v[0] for v in groups.values()}]
    masks += [set(v) for v in groups.values() if len(v)>1][:1]
    masks += [set(sum(list(groups.values())[:8],[]))]
    for mask in masks:
        physical=check(mask)
        print(f'PASS affinity={",".join(map(str,sorted(mask)))} physical={physical} logical={len(mask)}')
    check(original)
    failures=[]
    def worker(cpu):
        try:
            check({cpu})
            for _ in range(100):assert count(0)==1 and count(1)==1
        except BaseException as error:failures.append(error)
    threads=[threading.Thread(target=worker,args=(cpu,)) for cpu in cpus[:4]]
    for thread in threads:thread.start()
    for thread in threads:thread.join()
    assert not failures, failures
    assert os.sched_getaffinity(0)==original and count(0)==len(original)
    print('PASS per-thread affinity isolation')
    if '--benchmark' in sys.argv[2:]:
        start=time.perf_counter_ns()
        for _ in range(10000):count(0)
        print(f'10000 logical queries: {(time.perf_counter_ns()-start)/1e6:.3f} ms (includes ctypes)')
finally:
    os.sched_setaffinity(0,original)
