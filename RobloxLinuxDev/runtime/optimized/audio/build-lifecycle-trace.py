#!/usr/bin/env python3
"""Build an opt-in AudioToolbox lifecycle tracer without editing/installing Darling.

Requires the existing darling-o2 Ninja build and its objects. This adds logging
only and preserves dispatch behavior, including any applied invalid-handle fix.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Unexpected source revision: {old[:80]!r}')
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[2]
    build = runtime / 'src/darling-o2'
    source = runtime / 'src/darling/src/CoreAudio/AudioToolbox'
    if not (build / 'build.ninja').is_file():
        parser.error('Build the pinned Darling runtime first; darling-o2/build.ninja is missing')
    output = (args.output or Path(tempfile.mkdtemp(prefix='rbx-audio-trace-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error('--output must be an empty directory; existing artifacts are preserved')
    shutil.copytree(source, output / 'src')

    manager = output / 'src/AudioComponentManager.mm'
    text = manager.read_text()
    text = replace_once(text, '#include <cstdlib>', '''#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <pthread.h>
#include <execinfo.h>
static void lifecycleTrace(const char* op, void* id, void* object, int status) {
 static std::atomic<unsigned> count{0};
 bool missing = !strcmp(op, "lookup-missing");
 if (count.fetch_add(1) >= 4096 && !missing) return;
 fprintf(stderr, "AUDIO-LIFECYCLE thread=%p op=%s id=%p object=%p status=%d\\n",
         (void*)pthread_self(), op, id, object, status);
 if (missing || !strcmp(op, "dispose-enter")) {
  void* frames[16]; int n = backtrace(frames, 16);
  fprintf(stderr, "AUDIO-LIFECYCLE stack");
  for (int i=0; i<n; ++i) fprintf(stderr, " %p", frames[i]);
  fputc('\\n', stderr);
 }
}''')
    replacements = [
        ('OSStatus AudioComponentManager::instantiate(AudioComponent c, AudioComponentInstance* out)\n{',
         'OSStatus AudioComponentManager::instantiate(AudioComponent c, AudioComponentInstance* out)\n{\n lifecycleTrace("new-enter", c, nullptr, 0);'),
        ('cid.object = regd.factory(&regd.desc);',
         'cid.object = regd.factory(&regd.desc);\n lifecycleTrace("new-factory", *out, cid.object, cid.object ? noErr : invalidComponentID);'),
        ('OSStatus status = cid.object->Open(cid.object, *out);',
         'OSStatus status = cid.object->Open(cid.object, *out);\n lifecycleTrace("new-open", *out, cid.object, status);'),
        ('OSStatus AudioComponentManager::dispose(AudioComponentInstance instance)\n{',
         'OSStatus AudioComponentManager::dispose(AudioComponentInstance instance)\n{\n lifecycleTrace("dispose-enter", instance, nullptr, 0);'),
        ('OSStatus status = cid.object->Close(cid.object);',
         'OSStatus status = cid.object->Close(cid.object);\n lifecycleTrace("dispose-close", instance, cid.object, status);'),
        ('m_componentInstances.erase(it);',
         'm_componentInstances.erase(it);\n lifecycleTrace("dispose-erased", instance, nullptr, status);'),
    ]
    for old, new in replacements:
        text = replace_once(text, old, new)
    lookup = '''AudioComponentPlugInInterface* AudioComponentManager::instanceInterface(AudioComponentInstance instance)
{
	std::unique_lock<std::recursive_mutex> l2(m_componentInstancesMutex);
	auto it = m_componentInstances.find(instance);
	if (it == m_componentInstances.end())
		return nullptr;'''
    text = replace_once(text, lookup, lookup.replace('return nullptr;', '''{
 lifecycleTrace("lookup-missing", instance, nullptr, badComponentInstance);
 return nullptr;
 }'''))
    manager.write_text(text)

    dispatch = output / 'src/ComponentDispatch.h'
    text = replace_once(dispatch.read_text(), '#include <CarbonCore/Components.h>',
                        '#include <CarbonCore/Components.h>\n#include <cstdio>\n#include <atomic>\n#include <pthread.h>')
    text = replace_once(text, 'OSStatus dispatchCall(AudioComponentInstance inUnit, SInt16 sel, Args... args)\n{',
                        '''OSStatus dispatchCall(AudioComponentInstance inUnit, SInt16 sel, Args... args)
{
 if (sel==0x201 || sel==0x202 || sel==1 || sel==2) {
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1)<4096)
   fprintf(stderr, "AUDIO-LIFECYCLE thread=%p op=dispatch id=%p selector=%04x\\n",
           (void*)pthread_self(), inUnit, unsigned(sel));
 }''')
    dispatch.write_text(text)

    entries = json.loads(subprocess.check_output(['ninja', '-C', str(build), '-t', 'compdb'], text=True))
    objects = {}
    with (output / 'build.log').open('w') as log:
        for entry in entries:
            if not entry['file'].endswith(('/AudioToolbox/AUComponent.cpp', '/AudioToolbox/AudioComponentManager.mm')):
                continue
            command = shlex.split(entry['command'])
            target = str(output / (Path(entry['file']).name + '.o'))
            objects[command[command.index('-o') + 1]] = target
            command[command.index('-o') + 1] = target
            for option in ('-MF', '-MT'):
                if option in command:
                    command[command.index(option) + 1] = target + ('.d' if option == '-MF' else '')
            command[command.index(entry['file'])] = str(output / 'src' / Path(entry['file']).name)
            subprocess.run(command, cwd=entry['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
        if len(objects) != 2:
            raise RuntimeError('Expected exactly two AudioToolbox compilation commands')
        commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
                                            'src/CoreAudio/AudioToolbox/AudioToolbox'], text=True).splitlines()
        link = next(line for line in reversed(commands) if '-o src/CoreAudio/AudioToolbox/AudioToolbox ' in line)
        command = shlex.split(link)
        if command[:2] == [':', '&&']:
            command = command[2:]
        if command[-2:] == ['&&', ':']:
            command = command[:-2]
        if '&&' in command:
            raise RuntimeError('Unexpected compound link command')
        command = [objects.get(arg, arg) for arg in command]
        framework = output / 'AudioToolbox.framework'
        (framework / 'Versions/A').mkdir(parents=True)
        binary = framework / 'Versions/A/AudioToolbox'
        command[command.index('-o') + 1] = str(binary)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    (framework / 'Versions/Current').symlink_to('A')
    (framework / 'AudioToolbox').symlink_to('Versions/Current/AudioToolbox')
    print(f'Diagnostic framework: {framework}')
    print(f'SHA256: {hashlib.sha256(binary.read_bytes()).hexdigest()}')
    print(f'Compiler/linker diagnostics: {output / "build.log"}')
    print('For this launch only, set:')
    print(shlex.join(['env', 'DYLD_FRAMEWORK_PATH=/Volumes/SystemRoot' + str(output),
                      '/path/to/RobloxLinux.AppImage']))


if __name__ == '__main__':
    main()
