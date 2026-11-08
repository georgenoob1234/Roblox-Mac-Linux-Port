import sys,re,bisect,subprocess,collections,os
S=sys.argv[1]
maps=[]
for l in open(S+'/maps.txt'):
    p=l.split()
    if len(p)<6: continue
    a,b=[int(x,16) for x in p[0].split('-')]; maps.append((a,b,p[1],int(p[2],16),p[5]))
maps.sort()
base={}
for a,b,perm,off,path in maps:
    if 'x' in perm and path not in base: base[path]=a if not path.endswith('RobloxPlayer') else 0
starts=[m[0] for m in maps]
def mod(ip):
    i=bisect.bisect_right(starts,ip)-1
    if i>=0 and maps[i][0]<=ip<maps[i][1]: return maps[i][4]
    return '?'
syms={}
def symtab(path):
    if path in syms: return syms[path]
    t=[]
    if os.path.exists(path):
        try:
            out=subprocess.run(['llvm-nm','-n','--defined-only',path],capture_output=True,text=True).stdout
            for l in out.splitlines():
                p=l.split()
                if len(p)==3 and p[1] in 'tT': t.append((int(p[0],16),p[2]))
        except Exception: pass
    syms[path]=([x[0] for x in t],[x[1] for x in t]); return syms[path]
def sym(ip):
    m=mod(ip); short=m.split('/')[-1]
    if m not in base: return short+'+?'
    vm=ip-base[m]
    a,n=symtab(m)
    i=bisect.bisect_right(a,vm)-1
    if i<0 or not a: return f"{short}:{vm:#x}"
    return f"{short}:{n[i]}"
thr={}
for l in open(S+'/threads.txt'):
    p=l.split(None,2); thr[p[0]]=(int(p[1]),p[2].strip() if len(p)>2 else '')
rows=[]
for l in open(S+'/stacks.txt'):
    p=l.split(); rows.append((p[0],int(p[1]),[int(x,16) for x in p[2:]]))
SYS=('libsystem_kernel.dylib','libc.so.6','[vdso]','libsystem_pthread.dylib','librbxkqueue.dylib','libdyld.dylib','libsystem_platform.dylib')
def report(tid,label):
    tot=sum(c for t,c,_ in rows if t==tid)
    if not tot: return
    print(f"\n===== thread {tid} {label}: samples={tot} cpu_ticks={thr.get(tid,(0,''))[0]}")
    leaf=collections.Counter(); sysc=collections.Counter(); caller=collections.Counter()
    for t,c,st in rows:
        if t!=tid or not st: continue
        lm=mod(st[0]).split('/')[-1]; leaf[lm]+=c
        if lm in SYS:
            # chain of system-layer symbols until first non-system frame
            chain=[]; first=None
            for ip in st:
                m=mod(ip).split('/')[-1]
                if m in SYS: 
                    s=sym(ip).split(':')[-1]
                    if not chain or chain[-1]!=s: chain.append(s)
                else: first=sym(ip); break
            sysc[' <- '.join(chain[:4])]+=c; caller[(chain[-1] if chain else '?')+'  from  '+str(first)]+=c
    for m,c in leaf.most_common(8): print(f"  {c/tot*100:5.1f}%  leaf {m}")
    print("  -- system-layer chains:")
    for k,c in sysc.most_common(10): print(f"  {c/tot*100:5.1f}%  {k}")
    print("  -- entry points into system layer (outermost sys fn from caller):")
    for k,c in caller.most_common(10): print(f"  {c/tot*100:5.1f}%  {k}")
top=sorted(thr.items(),key=lambda x:-x[1][0])[:4]
for tid,(ticks,name) in top: report(tid,name)
# whole-process system layer totals
tot=sum(c for _,c,_ in rows); sysall=collections.Counter()
for t,c,st in rows:
    if st and mod(st[0]).split('/')[-1] in SYS:
        chain=[]
        for ip in st:
            m=mod(ip).split('/')[-1]
            if m in SYS:
                s=sym(ip).split(':')[-1]
                if not chain or chain[-1]!=s: chain.append(s)
            else: break
        sysall[chain[-1] if chain else '?']+=c
print(f"\n===== whole process: samples={tot}; system-layer entry fns:")
for k,c in sysall.most_common(15): print(f"  {c/tot*100:5.1f}%  {k}")

if len(sys.argv)>2:
    tid=sys.argv[2]
    full=collections.Counter()
    for t,c,st in rows:
        if t==tid: full[' > '.join(sym(ip) for ip in st[:10])]+=c
    tot=sum(full.values())
    for k,c in full.most_common(12): print(f"{c/tot*100:5.1f}% {k}\n")
