#!/usr/bin/env python3
"""Check CPU swap-return pacing, not GPU execution or compositor scanout."""
import argparse
import bisect
import csv
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path


def measure(times):
    if len(times)<2 or any(b<=a for a,b in zip(times,times[1:])):
        raise ValueError("need at least two strictly increasing timestamps for one context")
    gaps=[b-a for a,b in zip(times,times[1:])]
    ordered=sorted(gaps)
    run=longest=0
    for gap in gaps:
        run=run+gap if gap>1e9/60 else 0
        longest=max(longest,run)
    # A brief fast frame must not hide an otherwise slow half-second window.
    rolling=[2*(i-bisect.bisect_right(times,t-500_000_000)+1)
             for i,t in enumerate(times) if t-times[0]>=500_000_000]
    return dict(frames=len(gaps),seconds=(times[-1]-times[0])/1e9,
                average_fps=len(gaps)*1e9/(times[-1]-times[0]),
                p50_ms=ordered[math.ceil(len(gaps)*.5)-1]/1e6,
                p99_ms=ordered[math.ceil(len(gaps)*.99)-1]/1e6,
                max_ms=max(gaps)/1e6,
                over_240_budget=sum(gap>math.ceil(1e9/240) for gap in gaps),
                longest_below_60_run_s=longest/1e9,
                worst_half_second_fps=min(rolling) if rolling else None,
                sustained_60_failure=longest>500_000_000,
                strict_240_pass=max(gaps)<=math.ceil(1e9/240))


def check():
    smooth=measure([round(i*1e9/240) for i in range(481)])
    assert smooth['strict_240_pass'] and not smooth['sustained_60_failure']
    assert smooth['worst_half_second_fps']==240
    assert measure([0,501_000_000])['sustained_60_failure']
    assert not measure([0,500_000_000])['sustained_60_failure']
    slow=measure([i*20_000_000 for i in range(31)])
    assert slow['sustained_60_failure'] and slow['worst_half_second_fps']==50
    recovery=measure([0,300_000_000,304_000_000,604_000_000])
    assert not recovery['sustained_60_failure'] and recovery['worst_half_second_fps']<60
    for invalid in ([],[0],[0,0],[1,0]):
        try: measure(invalid)
        except ValueError: pass
        else: raise AssertionError(invalid)
    with tempfile.TemporaryDirectory() as directory:
        trace=Path(directory)/'frames.csv'
        trace.write_text('context,end_ns,swap_ns\n'+''.join(
            f'ctx,{round(i*1e9/240)},1000\n' for i in range(241))+'ctx,100')
        command=[sys.executable,__file__,str(trace)]
        result=subprocess.run(command,capture_output=True,text=True)
        assert result.returncode==0 and '"incomplete_tail": true' in result.stdout,result.stderr
        assert subprocess.run(command+['--duration','2'],capture_output=True).returncode==2
        trace.write_text('context,end_ns,swap_ns\n'+''.join(
            f'{"gl" if i%2 else "vk"},{round(i*1e9/240)},1000\n' for i in range(241)))
        assert subprocess.run(command,capture_output=True).returncode==2
        result=subprocess.run(command+['--all-contexts'],capture_output=True,text=True)
        assert result.returncode==0 and '"average_fps": 240.0' in result.stdout,result.stderr
        trace.write_text('context,end_ns,swap_ns\nctx,200,1\nctx,100,1\n')
        assert subprocess.run(command,capture_output=True).returncode==2
    print('PASS: frame pacing, exact thresholds, recovery, single stalls and invalid traces')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace',type=Path,nargs='?')
    selection=p.add_mutually_exclusive_group()
    selection.add_argument('--context',help='select one presentation context')
    selection.add_argument('--all-contexts',action='store_true',help='combine GL/Vulkan handoffs for one window; do not use for independent windows')
    p.add_argument('--start',type=float,default=0,help='seconds from first recorded swap; no implicit warm-up exclusion')
    p.add_argument('--duration',type=float,help='seconds to analyze after --start')
    p.add_argument('--self-test',action='store_true')
    args=p.parse_args()
    if args.self_test:check();return 0
    if not args.trace:p.error('trace required')
    if not math.isfinite(args.start) or args.start<0 or (args.duration is not None and (not math.isfinite(args.duration) or args.duration<=0)):
        p.error('start must be nonnegative and duration must be positive and finite')
    try:
        data=args.trace.read_text()
        incomplete=bool(data) and not data.endswith('\n')
        # A buffered live/crashed trace can end mid-row. Exclude that row
        # explicitly, never parse its truncated numeric fields as timestamps.
        if incomplete:data=data[:data.rfind('\n')+1]
        rows=list(csv.DictReader(data.splitlines()))
        contexts={row['context'] for row in rows}
        if not rows:raise ValueError('empty trace')
        if not args.context and not args.all_contexts and len(contexts)!=1:
            raise ValueError('select --context from '+', '.join(sorted(contexts)))
        context='all' if args.all_contexts else args.context or next(iter(contexts))
        selected=[row for row in rows if args.all_contexts or row['context']==context]
        times=[int(row['end_ns']) for row in selected]
        if not times:raise ValueError('context has no frames')
        if any(b<=a for a,b in zip(times,times[1:])):
            raise ValueError('non-monotonic trace; fix its clock or context ordering before benchmarking')
        if any(int(row['swap_ns'])<0 for row in selected):raise ValueError('negative swap duration')
        begin=times[0]+round(args.start*1e9)
        end=begin+round(args.duration*1e9) if args.duration is not None else times[-1]
        if end>times[-1]:raise ValueError('trace does not cover requested duration; a crash may have lost the buffered tail')
        result=measure([t for t in times if begin<=t<=end])
    except (OSError,ValueError,KeyError,TypeError) as error:p.error(str(error))
    print(json.dumps(dict(context=context,start_s=args.start,incomplete_tail=incomplete,**result),indent=2))
    print('Scope: recorded swap returns only; no GPU/scanout timing or guarantee beyond this interval.')
    return 0 if result['strict_240_pass'] and result['seconds']>=.5 else 1


if __name__=='__main__':raise SystemExit(main())
