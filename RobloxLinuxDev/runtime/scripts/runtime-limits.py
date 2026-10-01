#!/usr/bin/env python3
"""Bound libkqueue's FD scans and optionally select the configured CPU affinity."""
import os
import resource
import sys

hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
limit = 65536 if hard == resource.RLIM_INFINITY else min(hard, 65536)
resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))
if value := os.environ.get('ROBLOX_MAC_CPU_AFFINITY'):
    try:
        cpus = {int(cpu) for cpu in value.split(',')}
    except ValueError:
        raise SystemExit('ROBLOX_MAC_CPU_AFFINITY must be comma-separated CPU numbers')
    if not cpus or not cpus <= os.sched_getaffinity(0):
        raise SystemExit('Requested CPUs are outside this process affinity')
    os.sched_setaffinity(0, cpus)
if len(sys.argv) < 2:
    raise SystemExit('Missing runtime command')
os.execvp(sys.argv[1], sys.argv[1:])
