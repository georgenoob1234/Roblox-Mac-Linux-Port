#!/usr/bin/env python3
"""Generate real compressed samples and test the Darling decoder -> Metal bridge in isolation."""
import json, os, pathlib, re, shlex, struct, subprocess, tempfile
here=pathlib.Path(__file__).resolve().parent
root=here.parent
out=pathlib.Path(os.environ.get("OUT",str(here))).resolve()
state=pathlib.Path(tempfile.mkdtemp(prefix='roblox-video-'))
commands=[]
for codec,encoder,tag,extension in [('avc1','libx264','avc1','h264'),('hvc1','libx265','hvc1','hevc')]:
    movie=state/(codec+'.mp4');annex=state/(codec+'.'+extension)
    args=['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=64x48:rate=5','-frames:v','8','-c:v',encoder,'-g','8','-bf','2']
    if codec=='hvc1':args+=['-x265-params','pools=1:frame-threads=1:log-level=error','-tag:v',tag]
    subprocess.run(args+[str(movie)],check=True)
    subprocess.run(['ffmpeg','-v','error','-i',str(movie),'-c','copy','-bsf:v',extension+'_mp4toannexb','-f',extension,str(annex)],check=True)
    nals=[n for n in re.split(b'\x00\x00\x00?\x01',annex.read_bytes()) if n]
    sets=[n for n in nals if ((n[0]&31) in (7,8) if codec=='avc1' else ((n[0]>>1)&63) in (32,33,34))]
    sets=list(dict.fromkeys(sets))
    info=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v','-show_packets','-show_streams','-of','json',str(movie)]))
    assert info['streams'][0]['time_base'].startswith('1/')
    scale=int(info['streams'][0]['time_base'].split('/')[1]);packets=info['packets'];raw=movie.read_bytes()
    fixture=state/(codec+'.samples')
    with fixture.open('wb') as f:
        f.write(struct.pack('<II',int.from_bytes(codec.encode(),'big'),len(sets)))
        for n in sets:f.write(struct.pack('<I',len(n))+n)
        f.write(struct.pack('<II',len(packets),scale))
        for p in packets:
            size=int(p['size']);offset=int(p['pos'])
            f.write(struct.pack('<qqI',int(p['pts']),int(p['duration']),size)+raw[offset:offset+size])
    guest='/Volumes/SystemRoot'
    env=['env','ROBLOX_MAC_VIDEO_HELPER='+str(out/'libtracka-video-host.so'),'DYLD_FRAMEWORK_PATH='+guest+str(here/'native/frameworks'),'ROBLOX_MAC_SHADER_CACHE='+guest+str(state),'DISPLAY=:65535','WAYLAND_DISPLAY=','PTHREAD_MUTEX_DEFAULT_POLICY=3','PTHREAD_MUTEX_USE_ULOCK=1','DYLD_FORCE_FLAT_NAMESPACE=1','DYLD_LIBRARY_PATH='+guest+str(here/'native/lib')+':'+guest+str(here/'renderer')+':'+guest+str(root/'shims/metal'),
         'DYLD_INSERT_LIBRARIES='+':'.join(guest+str(root/p) for p in ('optimized/renderer/libtracka-present.dylib','shims/appkit/libappkitgaps.dylib','shims/metal/libmetalgaps.dylib','optimized/native/lib/librbxkqueue.dylib','shims/setsid/libnosetsid.dylib'))]
    for pf in ('y420','420v','BGRA'):
        commands.append(shlex.join(env+[guest+str(out/'video-check'),guest+str(fixture),pf]))
environment=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_DATA=str(state/'runtime'),ROBLOX_MAC_WAYLAND='0')
environment.pop('DISPLAY',None);environment.pop('WAYLAND_DISPLAY',None)
with (state/'run.log').open('w') as log:
    result=subprocess.run(['timeout','--kill-after=5s','50s',str(root/'bin/roblox-mac'),'--shell',' && '.join(commands)],env=environment,stdout=log,stderr=subprocess.STDOUT)
for line in (state/'run.log').read_text(errors='replace').splitlines():
    if line.startswith('PASS '):print(line)
print('Video test log:',state/'run.log')
raise SystemExit(result.returncode)
