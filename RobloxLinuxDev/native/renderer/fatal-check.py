#!/usr/bin/env python3
"""No renderer source may abort without saying why: every abort goes through indium/fatal.hpp."""
from pathlib import Path
import re

here = Path(__file__).resolve().parent
sources = [path for pattern in ('*.cpp', '*.mm', '*.hpp', '*.h', 'indium/*.hpp') for path in here.glob(pattern)
           if not path.name.endswith('-check.cpp') and path.name not in ('check.cpp', 'fatal.hpp')]
assert len(sources) > 20, sources
silent = [f'{path.relative_to(here)}:{number}: {line.strip()}'
          for path in sorted(sources) for number, line in enumerate(path.read_text().splitlines(), 1)
          if re.search(r'\b(abort|_Exit|_exit|quick_exit|terminate)\s*\(', line)]
assert not silent, 'renderer code exits without a reason:\n' + '\n'.join(silent)
reported = sum(len(re.findall(r'\bINDIUM_(?:VULKAN_)?FATAL\(', path.read_text())) for path in sources)
assert reported >= 40, reported
print(f'PASS no silent abort in {len(sources)} renderer sources; {reported} fatal paths report their cause')
