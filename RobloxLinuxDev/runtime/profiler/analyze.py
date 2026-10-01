#!/usr/bin/env python3
"""Summarize an F9 capture without opening Roblox or a browser."""
import json, sys
from collections import Counter
from pathlib import Path

def analyze(path):
    data=json.loads(Path(path).read_text());d=data.get('diagnostics',{})
    warning=data.get('measurement_warning')
    if warning:print('WARNING: '+warning)
    elif d.get('profiler_work',{}).get('overlay_visible'):
        print('WARNING: HUD visible in this legacy capture; frame times include overlay overhead. Do not use as an FPS baseline.')
    else:print('NOTE: Diagnostic timings include profiler instrumentation overhead.')
    if data.get('cpu_scope_timing') is False:
        print('PC sampling mode: nested CPU/draw scopes and their call counts are disabled.')
    frames=[e['args']['duration_ms'] for e in data['traceEvents'] if e['name']=='CPU swap-return interval']
    if frames:
        ordered=sorted(frames)
        q=lambda p:ordered[min(len(ordered)-1,(len(ordered)*p+99)//100-1)]
        longest=run=0
        for ms in frames:
            run=run+ms if ms>1000/60 else 0;longest=max(longest,run)
        mean=sum(frames)/len(frames)
        print(f'Frame intervals: {1000/mean:.1f} FPS; mean {mean:.3f}, p50 {q(50):.3f}, p95 {q(95):.3f}, p99 {q(99):.3f}, max {max(frames):.3f} ms')
        print(f'Over 4.17ms: {sum(x>1000/240 for x in frames)}/{len(frames)}; longest consecutive >16.67ms run: {longest:.1f}ms')
        spans=[e for e in data['traceEvents'] if e.get('cat')=='renderer_diagnostic' and e.get('ph')=='X']
        if health:=d.get('renderer_diagnostics'):
            print(f'Renderer diagnostics: {health["retained"]} retained, {health["overwritten"]} overwritten, {health["contention_drops"]} contention drops; missing spans are not zero work.')
        if spans:
            worst=max((e for e in data['traceEvents'] if e['name']=='CPU swap-return interval'),key=lambda e:e['args']['duration_ms'])
            end=worst['ts'];begin=end-worst['args']['duration_ms']*1000
            overlapping=[e for e in spans if e['ts']<end and e['ts']+e['dur']>begin]
            print('Renderer spans overlapping worst swap interval (full inclusive wall/CPU; nested spans are not additive):')
            for e in sorted(overlapping,key=lambda e:e['dur'],reverse=True)[:12]:
                a=e['args'];cpu=a.get('cpu_ms')
                cpu_text=f'{cpu:.3f}' if cpu is not None else '--'
                print(f'  {e["dur"]/1000:.3f} wall ms, {cpu_text} CPU ms; TID {e["tid"]}; {e["name"]}; bytes={a.get("bytes",0)}, object={a.get("object")}, detail={a.get("detail",0)}')
    if not d:
        print('Legacy capture: no diagnostic snapshot.');return
    if policy:=d.get('requested_egl_interval'):
        print(f'Requested EGL interval: {policy}; not a scanout measurement')
    if memory:=d.get('memory'):
        if memory['available']:
            if 'roblox_estimate_mb' in memory:
                print(f'Roblox RAM estimate: {memory["roblox_estimate_mb"]:.2f} MB; identified Roblox libraries/assets {memory["roblox_identified_mb"]:.2f} MB')
                print('  '+memory['roblox_estimate_method'])
            print(f'Memory: Roblox + runtime RAM {memory["ram_mb"]:.2f} MB; total mapped RSS {memory["rss_mb"]:.2f} MB')
            for row in memory['resident_categories']:print(f'  {row["rss_mb"]:.2f} MB resident: {row["name"]}')
            print(f'  RAM proportional share {memory["ram_pss_mb"]:.2f} MB; swap {memory["swap_mb"]:.2f} MB; virtual address space {memory["virtual_mb"]:.2f} MB')
        else:print('Memory breakdown unavailable (smaps unreadable / incomplete).')
        print(f'  Profiler UI allocations {memory["profiler_ui_mb"]:.2f} MB; peak {memory["profiler_ui_peak_mb"]:.2f} MB')
        print('  '+memory['method']+' MB = 1,000,000 bytes.')
    if work:=d.get('profiler_work'):
        print(f'Profiler: overlay {"visible" if work["overlay_visible"] else "hidden"}; {work["layout_builds"]} layouts / {work["cached_draws"]} cached draws; symbol resolution {work["symbol_resolution_ms"]:.3f}ms, {work["symbol_cache_misses"]} misses')
    print(f'User-PC samples: {d["samples"]}; lost {d["lost"]}; capacity drops {d["ip_capacity_drops"]}; active {d["active_seconds"]:.1f}s; sampler errno {d["sampler_error"]}')
    total=d.get('total_thread_cpu_ms',0)
    if total:print(f'Busy-thread CPU coverage: {100*d["covered_thread_cpu_ms"]/total:.1f}%')
    for t in sorted(d['threads'],key=lambda t:t['cpu_ms'],reverse=True)[:8]:
        print(f'TID {t["tid"]} (procfs {t.get("proc_tid",t["tid"])}) {t["name"]}: {t["cpu_ms"]:.1f} CPU ms, {t.get("samples",0)} PCs; perf errno {t.get("perf_error",0)}; hot PC {t.get("top_ip","unavailable")}')
    categories=Counter();hot=Counter();host_modules=Counter()
    window_frames=d.get('frame_window_count',0)
    def cpu_per_frame(count):
        if not window_frames or d.get('frame_window_truncated'):return '-- CPU ms/frame'
        return f'{count*d["ip_sample_period_ns"]/1e6/window_frames:.3f} CPU ms/frame'
    for row in d['ips']:
        categories[row['category']]+=row['count']
        hot[(row['module'],row['symbol'] or f'+0x{row["offset"]:x}')]+=row['count']
        if row['category']==2:host_modules[row['module']]+=row['count']
    categories[3]+=d['ip_capacity_drops']
    if d['samples']:
        for i,name in enumerate(('guest native','compatibility runtime','host libraries','unaccounted')):
            print(f'  {name}: {100*categories[i]/d["samples"]:.2f}%')
        if estimates:=d.get('category_cpu_estimates'):
            label=f'{window_frames} graph frames' if window_frames else f'{d["active_seconds"]:.1f}s active (legacy capture)'
            print(f'Estimated user CPU over {label}; not frame latency:')
            for group in estimates:
                if group['estimated_cpu_ms'] is None:continue
                print(f'  {group["name"]}: {group["share_percent"]:.2f}%; {cpu_per_frame(group["samples"])}; {group["estimated_cpu_ms"]:.1f} sampled CPU ms')
        print('Host-library modules (same graph window; not critical-path latency):')
        for module,count in host_modules.most_common():print(f'  {cpu_per_frame(count)} | {count:5d} samples | {module}')
        print('Top sampled symbols (self CPU estimates, not inclusive call timings):')
        for (module,symbol),count in hot.most_common(15):print(f'  {100*count/d["samples"]:6.2f}% {cpu_per_frame(count)} | {count} samples | {module} {symbol}')
    stages=[s for s in d['stages'] if s.get('cpu_tracked',True)] if data.get('cpu_scope_timing') is not False else []
    print('Top exclusive instrumented CPU stages (recent scope window):' if stages else 'Instrumented CPU stages: unavailable in this capture.')
    denominator=d.get('runner_cpu_window_ms')
    for s in sorted(stages,key=lambda s:s['cpu_ms'],reverse=True)[:12]:
        share=f'{100*s["cpu_ms"]/denominator:.2f}%' if denominator else '--'
        mean=s['total_ms']/s['calls'] if s['calls'] else 0
        print(f'  {s["cpu_ms"]:8.3f} CPU ms ({share}); wall avg {mean:.3f}, max {s["max_ms"]:.3f}ms; cores {s["cores"]}; {s["name"]}')
    print('CPU and wall windows differ; overlapping wall spans are not additive. Hidden-overlay measurements are required for final FPS comparisons.')

if __name__=='__main__':
    if len(sys.argv)!=2:raise SystemExit('usage: analyze.py /path/to/compat-*.json')
    analyze(sys.argv[1])
