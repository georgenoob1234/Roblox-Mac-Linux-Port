#!/usr/bin/env python3
"""Triage the remaining metal2vulkan / prepare-cache shader failures.

Two stages, matching how the pipeline produces evidence:

  --collect OUTDIR
      Scan a translate-all.sh output tree (OUTDIR/air, OUTDIR/spv, OUTDIR/repros) and write
      OUTDIR/shader-failures.json: one machine-readable record per shader whose translation
      did not produce a validated module plus reflection, with the full translator error
      text, the repro bundle path and the time/RSS measurements. translate-all.sh runs this
      automatically. Collection is read-only; it never rewrites SPIR-V, AIR or caches.

  (default) analysis
      Ingest OUTDIR/shader-failures.json plus the spv-cache-v1 report.json written by
      prepare-cache.py, correlate both per shader index, bucket every failure into an
      actionable root-cause category and print (or write) a text manifest that points each
      bucket at the fix recipe in SHADER-FAILURE-PLAN.md.

  --self-test
      Build synthetic fixtures in a temporary directory and assert the collector and every
      category rule, so future edits to this script stay honest.

The analyzer never marks a failed shader as supported. Exit status: 0 every failure was
bucketed, 1 at least one failure is 'uncategorized' (extend the tables below first), 2 the
inputs could not be read.
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import struct
import sys
import tempfile

CATEGORY_ORDER = ['air-unsupported', 'spirv-validation', 'descriptor-layout',
                  'resource-limit', 'signature-mismatch', 'uncategorized']
CATEGORY_TITLES = {
    'air-unsupported': 'AIR translation: missing opcode/intrinsic',
    'spirv-validation': 'SPIR-V validation error',
    'descriptor-layout': 'descriptor set/layout mismatch (Indium descriptor ABI)',
    'resource-limit': 'resource limit exceeded (time/RSS ceiling or killed tool)',
    'signature-mismatch': 'signature/reflection mismatch',
    'uncategorized': 'unrecognized failure text (extend the category tables)',
}
CATEGORY_PLAN = {
    'air-unsupported': 'SHADER-FAILURE-PLAN.md section 2.1 (metal2vulkan emitter)',
    'spirv-validation': 'SHADER-FAILURE-PLAN.md section 2.2 (SPIR-V metadata fix)',
    'descriptor-layout': 'SHADER-FAILURE-PLAN.md section 2.3 (prepare-cache.py remap)',
    'resource-limit': 'SHADER-FAILURE-PLAN.md section 2.4 (translator limits/crash)',
    'signature-mismatch': 'SHADER-FAILURE-PLAN.md section 2.5 (reflection/signature)',
    'uncategorized': 'SHADER-FAILURE-PLAN.md section 2.6 (extend this script)',
}
LOG_LIMIT = 16000

# --- translation-side categorization ---------------------------------------------------------

STAGE_SIGNATURE = ('stage auto-detect failed',
                   'no !air.vertex/!air.fragment/!air.kernel stage metadata')
EMIT_META_FALLBACKS = ('emit-meta failed', 'emit-meta unsupported for stage')
AIR_PATTERNS = [re.compile(p, re.IGNORECASE) for p in (
    r'native emitter:',
    r'\bunknown\b.*\b(?:air|intrinsic|opcode|instruction)\b',
    r'\b(?:unhandled|unsupported)\b.*\b(?:air|intrinsic|opcode|call|instruction)\b',
    r'\bair\.[a-z][a-z0-9_.]*',
    r'expects \d+(?: or \d+)? operands',
    r'llvm-dis failed:',
    r'\bintrinsic\b',
)]

# --- adaptation-side categorization (exact strings raised by prepare-cache.py) ----------------

ADAPT_SPIRV = ('spirv-val rejected the adapted module', 'unaligned SPIR-V',
               'invalid SPIR-V')
ADAPT_DESCRIPTOR = ('imageblock descriptors unsupported', 'non-descriptor resource unsupported',
                    'descriptor array/set unsupported', 'texel buffer unsupported',
                    'unexpected descriptor set', 'invalid tag length')
ADAPT_SIGNATURE = ('invalid metallib', 'invalid function table', 'multiple entry points unsupported',
                   'entry-point name mismatch', 'unsupported varying semantic',
                   'duplicate varying semantic', 'varying location absent from reflection',
                   'Expecting value', 'Extra data', 'Unterminated string')


def parse_log(text):
    stage = None
    pass_line = False
    fallback = None
    error_lines = []
    breach = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('metal2vulkan: auto-detected stage '):
            stage = s.rsplit(' ', 1)[-1]
        elif 'spirv-val vulkan1.2: PASS' in s and s.startswith('metal2vulkan: wrote '):
            pass_line = True
        elif s.startswith('metal2vulkan: FALLBACK (') and s.endswith(')'):
            fallback = s[len('metal2vulkan: FALLBACK ('):-1]
        elif s in ('20-second translation ceiling exceeded',
                   '500-MiB process-group RSS ceiling exceeded'):
            breach = s
        elif s.startswith('metal2vulkan: ') and not s.startswith('metal2vulkan: wrote '):
            error_lines.append(s)
    return stage, pass_line, fallback, error_lines, breach


def tool_kills(text):
    return [s for s in (line.strip() for line in text.splitlines())
            if 'killed by signal' in s or '[no verdict]' in s or 'timed out after' in s]


def evidence_text(log_text, repro_detail):
    lines = log_text.splitlines()
    start = 0
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('metal2vulkan: ') and not s.startswith('metal2vulkan: wrote ') \
                and not s.startswith('metal2vulkan: auto-detected stage '):
            start = i
            break
    body = '\n'.join(lines[start:])[:LOG_LIMIT]
    if repro_detail:
        body += '\n[repro detail]\n' + repro_detail[:LOG_LIMIT]
    return body


def scan_repros(repro_root):
    out = {}
    for failure in sorted(repro_root.glob('metal2vulkan-repro-*/failure.txt')):
        try:
            text = failure.read_text(errors='replace')
        except OSError:
            continue
        fields = {}
        for line in text.splitlines():
            for key in ('why', 'source', 'requested_stage', 'effective_stage'):
                if line.startswith(key + ': '):
                    fields[key] = line[len(key) + 2:].strip()
        source = fields.get('source', '')
        match = re.search(r'(\d+)\.air$', source)
        if not match:
            continue
        marker = '\ndetail:\n'
        pos = text.find(marker)
        detail = text[pos + len(marker):].strip() if pos >= 0 else ''
        out[int(match.group(1))] = {
            'repro_dir': str(failure.parent),
            'repro_why': fields.get('why'),
            'repro_effective_stage': fields.get('effective_stage'),
            'repro_detail': detail,
        }
    return out


def collect(out_dir):
    out = pathlib.Path(out_dir)
    spv, air, repros = out / 'spv', out / 'air', out / 'repros'
    if not spv.is_dir():
        raise SystemExit(f'no translation output directory: {spv}')
    repro_map = scan_repros(repros) if repros.is_dir() else {}
    failures = []
    ok = 0
    for log_path in sorted(spv.glob('*.out')):
        if not log_path.stem.isdigit():
            continue
        index = int(log_path.stem)
        text = log_path.read_text(errors='replace')
        stage, pass_line, fallback, error_lines, breach = parse_log(text)
        elapsed = peak = None
        time_path = spv / f'{log_path.stem}.time'
        if time_path.is_file():
            parts = time_path.read_text().split()
            if len(parts) == 2:
                try:
                    elapsed, peak = float(parts[0]), int(parts[1])
                except ValueError:
                    pass
        record = {
            'index': index,
            'stage': stage,
            'fallback': fallback,
            'error_lines': error_lines,
            'breach': breach,
            'tool_kills': tool_kills(text),
            'has_spv': (spv / f'{log_path.stem}.spv').is_file(),
            'has_json': (spv / f'{log_path.stem}.json').is_file(),
            'elapsed_seconds': elapsed,
            'peak_rss_kib': peak,
        }
        repro = repro_map.get(index)
        if repro:
            record.update(repro)
            record['stage'] = stage or repro.get('repro_effective_stage')
        if record['has_json'] and pass_line and not breach:
            ok += 1
        else:
            record['evidence'] = evidence_text(text, (repro or {}).get('repro_detail'))
            failures.append(record)
    report = {
        'collector_version': 1,
        'generated_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'out_dir': str(out.resolve()),
        'counts': {
            'air_files': len(list(air.glob('*.air'))) if air.is_dir() else 0,
            'translation_logs': ok + len(failures),
            'translated_ok': ok,
            'translation_failures': len(failures),
        },
        'failures': failures,
    }
    dest = out / 'shader-failures.json'
    dest.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{ok} translated, {len(failures)} failed -> {dest}")
    return dest


def categorize_translation(rec):
    if rec.get('breach') or rec.get('tool_kills'):
        return 'resource-limit'
    text = rec.get('evidence') or ''
    fallback = rec.get('fallback') or ''
    if 'spirv-val failed:' in text:
        return 'spirv-validation'
    if any(m in text or m in fallback for m in STAGE_SIGNATURE) or fallback in EMIT_META_FALLBACKS:
        return 'signature-mismatch'
    if any(p.search(text) for p in AIR_PATTERNS):
        return 'air-unsupported'
    return 'uncategorized'


def categorize_adaptation(error):
    e = error.strip()
    if any(s in e for s in ADAPT_SPIRV):
        return 'spirv-validation'
    if 'spirv-val' in e and ('returned non-zero exit status' in e or 'failed' in e):
        return 'spirv-validation'
    if any(s in e for s in ADAPT_DESCRIPTOR) or e.startswith('unsupported binding '):
        return 'descriptor-layout'
    if any(s in e for s in ADAPT_SIGNATURE):
        return 'signature-mismatch'
    return 'uncategorized'


def entry_names_from_pack(pack_path, indices):
    """Entry-point names keyed by shader index, read from the pack's metallib function tables."""
    data = pathlib.Path(pack_path).read_bytes()
    names = {}
    for index, m in enumerate(re.finditer(b'MTLB', data)):
        if index not in indices:
            continue
        size = struct.unpack_from('<Q', data, m.start() + 16)[0]
        blob = data[m.start():m.start() + size]
        try:
            names[index] = metallib_entry_name(blob)
        except ValueError:
            names[index] = None
    return names


def metallib_entry_name(blob):
    if len(blob) < 88 or blob[:4] != b'MTLB':
        raise ValueError('invalid metallib')
    size, offset, length = struct.unpack_from('<QQQ', blob, 16)
    if size != len(blob) or offset + length > size:
        raise ValueError('invalid function table')
    if struct.unpack_from('<I', blob, offset)[0] != 1:
        raise ValueError('multiple entry points unsupported')
    pos = offset + 8
    while pos + 4 <= offset + length:
        tag = blob[pos:pos + 4]
        pos += 4
        if tag == b'ENDT':
            break
        n = struct.unpack_from('<H', blob, pos)[0]
        pos += 2
        if pos + n > size:
            raise ValueError('invalid tag length')
        if tag == b'NAME':
            return blob[pos:pos + n].rstrip(b'\0').decode(errors='replace')
        pos += n
    return None


def summarize_reflection(spv_dir, index):
    """One-line-per-binding view of the translator's reflection for an adaptation failure."""
    path = spv_dir / f'{index:04d}.json'
    try:
        meta = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    parts = []
    if meta.get('entry_point'):
        parts.append(f"entry {meta['entry_point']}")
    if meta.get('stage'):
        parts.append(f"stage {meta['stage']}")
    for b in meta.get('bindings', []):
        desc = b.get('descriptor')
        loc = f"set {desc['set']} binding {desc['binding']}" if desc else 'no descriptor'
        shape = (b.get('texture_shape') or {}).get('dimension')
        parts.append(f"binding kind={b.get('kind')} metal_index={b.get('metal_index')} {loc}"
                     + (f" dim={shape}" if shape else ''))
    for v in meta.get('varyings', []):
        parts.append(f"varying location={v.get('location')} semantic={v.get('user_semantic')}")
    if meta.get('implicit_imageblock_attachments'):
        parts.append(f"implicit_imageblock_attachments={len(meta['implicit_imageblock_attachments'])}")
    if meta.get('fragment_imageblock'):
        parts.append('fragment_imageblock present')
    return '\n'.join(parts[:40])


def analyze(failures_path, report_path, pack_path=None):
    fx = json.loads(pathlib.Path(failures_path).read_text())
    rep = json.loads(pathlib.Path(report_path).read_text())
    tmap = {rec['index']: rec for rec in fx.get('failures', []) if isinstance(rec, dict)}
    rmap = {}
    warnings = []
    for entry in rep.get('failures', []):
        try:
            rmap[int(entry[0])] = str(entry[1])
        except (TypeError, ValueError, IndexError):
            warnings.append(f'unreadable report failure entry: {entry!r}')
    counts = fx.get('counts', {})
    if counts.get('air_files') not in (None, rep.get('total')):
        warnings.append(f"corpus mismatch: {counts.get('air_files')} AIR files were translated but "
                        f"report.json totals {rep.get('total')}; the two files describe different runs")
    for index in tmap:
        if index not in rmap:
            warnings.append(f'shader {index:04d} failed translation but report.json does not list it; '
                            'the report predates or postdates the collected run')
    spv_dir = pathlib.Path(fx.get('out_dir', '')) / 'spv'
    triage = []
    for index, rec in sorted(tmap.items()):
        triage.append({
            'index': index,
            'source': 'translation',
            'category': categorize_translation(rec),
            'stage': rec.get('stage'),
            'repro_dir': rec.get('repro_dir'),
            'fallback': rec.get('fallback'),
            'downstream_error': rmap.get(index),
            'elapsed_seconds': rec.get('elapsed_seconds'),
            'peak_rss_kib': rec.get('peak_rss_kib'),
            'evidence': rec.get('evidence'),
        })
    for index, error in sorted(rmap.items()):
        if index in tmap:
            continue  # translation failure dominates; its downstream error is already recorded
        triage.append({
            'index': index,
            'source': 'adaptation',
            'category': categorize_adaptation(error),
            'stage': None,
            'repro_dir': None,
            'fallback': None,
            'report_error': error,
            'reflection': summarize_reflection(spv_dir, index) if spv_dir.is_dir() else None,
        })
    names = entry_names_from_pack(pack_path, {t['index'] for t in triage}) if pack_path else {}
    manifest = render_manifest(fx, rep, triage, warnings, names, failures_path, report_path)
    return manifest, triage, warnings


def render_manifest(fx, rep, triage, warnings, names, failures_path, report_path):
    counts = fx.get('counts', {})
    adaptation_only = sum(1 for t in triage if t['source'] == 'adaptation')
    lines = ['Roblox shader failure triage', '============================']
    lines.append(f"collected : {fx.get('generated_utc', '?')} (translate-all.sh output: {fx.get('out_dir', '?')})")
    lines.append(f"failures  : {failures_path}")
    lines.append(f"report    : {report_path} (pack sha256 {rep.get('pack_sha256', '?')}, "
                 f"prepared {rep.get('prepared', '?')}/{rep.get('total', '?')})")
    lines.append(f"corpus    : {rep.get('total', '?')} shaders; translated OK {counts.get('translated_ok', '?')}; "
                 f"translation failures {counts.get('translation_failures', '?')}; "
                 f"adaptation-only failures {adaptation_only}; failing total {len(triage)}")
    lines.append('warnings  : ' + ('; '.join(warnings) if warnings else 'none'))
    lines.append('')
    lines.append('category counts (root causes):')
    for category in CATEGORY_ORDER:
        n = sum(1 for t in triage if t['category'] == category)
        if n:
            lines.append(f"  {category:<20} {n:>3}   {CATEGORY_TITLES[category]}")
    lines.append('')
    for category in CATEGORY_ORDER:
        bucket = [t for t in triage if t['category'] == category]
        if not bucket:
            continue
        lines.append(f"CATEGORY: {CATEGORY_TITLES[category]}  [{len(bucket)} shader(s)]")
        lines.append(f"  fix recipe: {CATEGORY_PLAN[category]}")
        lines.append('  ' + '-' * 72)
        for t in bucket:
            entry = names.get(t['index'])
            head = f"  shader {t['index']:04d}"
            if t.get('stage'):
                head += f"   stage={t['stage']}"
            head += f"   entry={entry if entry else '?'}"
            lines.append(head)
            if t['source'] == 'translation':
                if t.get('downstream_error'):
                    lines.append(f"    origin     : metal2vulkan translation failed "
                                 f"(prepare-cache downstream: {t['downstream_error']})")
                else:
                    lines.append('    origin     : metal2vulkan translation failed')
                if t.get('repro_dir'):
                    lines.append(f"    repro      : {t['repro_dir']}")
                if t.get('fallback'):
                    lines.append(f"    fallback   : {t['fallback']}")
                if t.get('elapsed_seconds') is not None:
                    lines.append(f"    limits     : elapsed {t['elapsed_seconds']:.2f}s, "
                                 f"peak RSS {t.get('peak_rss_kib')} KiB")
            else:
                lines.append('    origin     : prepare-cache adaptation failed (translation succeeded)')
                if t.get('report_error'):
                    lines.append(f"    error      : {t['report_error']}")
                if t.get('reflection'):
                    lines.append('    reflection :')
                    lines.extend(f"      {s}" for s in t['reflection'].splitlines())
                lines.append(f"    reflection file: {fx.get('out_dir', '?')}/spv/{t['index']:04d}.json")
            if t.get('evidence'):
                lines.append('    evidence:')
                lines.extend(f"      {s}" for s in t['evidence'].splitlines()[:40])
            lines.append('')
    lines.append('next steps:')
    lines.append('  1. Apply the per-category fix recipes in SHADER-FAILURE-PLAN.md (same directory).')
    lines.append('  2. Reproduce and re-translate one shader at a time from its repro directory '
                 '(plan section 3.1).')
    lines.append('  3. Re-run the full corpus and this triage after every fix (plan section 3.3).')
    return '\n'.join(lines) + '\n'


def self_test():
    with tempfile.TemporaryDirectory(prefix='shader-triage-') as tmp:
        out = pathlib.Path(tmp) / 'out'
        (out / 'spv').mkdir(parents=True)
        (out / 'air').mkdir()
        for i in range(12):
            (out / 'air' / f'{i:04d}.air').write_bytes(b'MTLB-placeholder')
        (out / 'repros').mkdir()
        repro = out / 'repros' / 'metal2vulkan-repro-42-1'
        repro.mkdir()
        (repro / 'failure.txt').write_text(
            f"why: translate failed\nsource: {out}/air/0100.air\noutput: {out}/spv/0100.spv\n"
            "requested_stage: auto\neffective_stage: fragment\n"
            "original_command: metal2vulkan input.air out.spv\n\n"
            "detail:\nnative emitter: air.atomic.global.add.u.i32 expects 4 or 5 operands\n")

        def log(name, text, time='3.0000 123456\n'):
            (out / 'spv' / name).with_suffix('.out').write_text(text)
            (out / 'spv' / name).with_suffix('.time').write_text(time)

        log('0100.out', 'metal2vulkan: auto-detected stage fragment\n'
                        'metal2vulkan: native emitter: air.atomic.global.add.u.i32 expects 4 or 5 operands\n'
                        'metal2vulkan: FALLBACK (translate failed)\n'
                        f'metal2vulkan: wrote repro {repro}\n')
        log('0200.out', 'metal2vulkan: auto-detected stage fragment\n'
                        'metal2vulkan: spirv-val failed:\n'
                        'error: 12: OpImageSampleImplicitLod: Expected Image to be of type OpTypeImage\n'
                        'metal2vulkan: FALLBACK (translate failed)\n')
        log('0300.out', 'metal2vulkan: auto-detected stage kernel\n'
                        '500-MiB process-group RSS ceiling exceeded\n', time='20.0151 512001\n')
        log('0400.out', 'metal2vulkan: auto-detected stage kernel\n'
                        'metal2vulkan: llvm-dis killed by signal [no verdict]:\n')
        log('0500.out', 'metal2vulkan: no !air.vertex/!air.fragment/!air.kernel stage metadata in module\n'
                        'metal2vulkan: FALLBACK (stage auto-detect failed)\n')
        for name in ('0600', '0700', '0800', '0900'):
            (out / 'spv' / f'{name}.out').write_text(
                'metal2vulkan: auto-detected stage vertex\n'
                f'metal2vulkan: wrote {out}/spv/{name}.spv; spirv-val vulkan1.2: PASS\n'
                f'metal2vulkan: wrote reflection {out}/spv/{name}.json\n')
            (out / 'spv' / f'{name}.time').write_text('1.0000 65000\n')
            (out / 'spv' / f'{name}.spv').write_bytes(b'\x03\x02\x23\x07')
            (out / 'spv' / f'{name}.json').write_text(
                '{"entry_point":"xPS","stage":"Vertex","bindings":[],"varyings":[]}')

        failures_path = collect(out)
        fx = json.loads(failures_path.read_text())
        assert fx['counts']['translated_ok'] == 4, fx['counts']
        assert fx['counts']['translation_failures'] == 5, fx['counts']
        by_index = {rec['index']: rec for rec in fx['failures']}
        assert by_index[100]['repro_dir'] == str(repro)
        assert 'air.atomic.global.add' in by_index[100]['evidence']

        report_path = pathlib.Path(tmp) / 'report.json'
        report_path.write_text(json.dumps({
            'prepared': 3, 'total': 12, 'pack_sha256': 'deadbeef',
            'failures': [
                [100, f"[Errno 2] No such file or directory: '{out}/spv/0100.json'"],
                [200, f"[Errno 2] No such file or directory: '{out}/spv/0200.json'"],
                [300, f"[Errno 2] No such file or directory: '{out}/spv/0300.json'"],
                [400, f"[Errno 2] No such file or directory: '{out}/spv/0400.json'"],
                [500, f"[Errno 2] No such file or directory: '{out}/spv/0500.json'"],
                [700, 'entry-point name mismatch'],
                [800, 'descriptor array/set unsupported'],
                [900, "Command '['spirv-val', '--target-env', 'vulkan1.2', '/x.spv']' "
                      "returned non-zero exit status 1."],
            ],
        }))
        manifest, triage, warnings = analyze(failures_path, report_path)
        cats = {t['index']: t['category'] for t in triage}
        expected = {100: 'air-unsupported', 200: 'spirv-validation', 300: 'resource-limit',
                    400: 'resource-limit', 500: 'signature-mismatch', 700: 'signature-mismatch',
                    800: 'descriptor-layout', 900: 'spirv-validation'}
        assert cats == expected, cats
        sources = {t['index']: t['source'] for t in triage}
        assert sources[100] == 'translation' and sources[700] == 'adaptation'
        assert not warnings, warnings
        assert 'shader 0100' in manifest and 'CATEGORY: AIR translation' in manifest
        assert 'fix recipe: SHADER-FAILURE-PLAN.md section 2.1' in manifest

        pack = pathlib.Path(tmp) / 'mini.pack'
        pack.write_bytes(make_mtlb('firstPS') + make_mtlb('secondVS'))
        names = entry_names_from_pack(pack, {0, 1})
        assert names == {0: 'firstPS', 1: 'secondVS'}, names

        # A report from a different run: the corpus-size mismatch warns, and an error
        # string no table knows lands in 'uncategorized' (main() exits 1 for that).
        report_path.write_text(json.dumps({
            'prepared': 10, 'total': 20, 'pack_sha256': 'other',
            'failures': [[1000, 'flux capacitor discharged']]}))
        manifest, triage, warnings = analyze(failures_path, report_path)
        assert {t['index']: t['category'] for t in triage}[1000] == 'uncategorized'
        assert any('corpus mismatch' in w for w in warnings)
    print('self-test PASS')


def make_mtlb(name):
    table = struct.pack('<I', 1) + b'\x00' * 4 + b'NAME' + struct.pack('<H', len(name)) \
        + name.encode() + b'ENDT'
    offset = 96
    blob = b'MTLB' + b'\x00' * 12 + struct.pack('<QQQ', offset + len(table), offset, len(table))
    return blob + b'\x00' * (offset - len(blob)) + table


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--collect', metavar='OUTDIR',
                        help='scan a translate-all.sh output tree and write OUTDIR/shader-failures.json')
    parser.add_argument('--failures', type=pathlib.Path,
                        help='shader-failures.json written by --collect (default: the '
                             'translate-all.sh out tree, or $ROBLOX_MAC_SHADER_BUILD)')
    parser.add_argument('--report', type=pathlib.Path,
                        help='report.json written by prepare-cache.py (default: the dev cache)')
    parser.add_argument('--pack', type=pathlib.Path,
                        help='optional shaders_metal_osx.pack; resolves entry-point names')
    parser.add_argument('--manifest', type=pathlib.Path,
                        help='also write the text manifest to this path')
    parser.add_argument('--json', type=pathlib.Path,
                        help='also write the structured triage to this path')
    parser.add_argument('--self-test', action='store_true',
                        help='run the built-in fixture tests and exit')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.collect:
        collect(args.collect)
        return 0
    here = pathlib.Path(__file__).resolve().parent
    failures = args.failures or pathlib.Path(
        os.environ.get('ROBLOX_MAC_SHADER_BUILD') or here / 'out') / 'shader-failures.json'
    report = args.report or here / '../data/spv-cache-v1/report.json'
    for path in (failures, report):
        if not path.is_file():
            print(f'missing input: {path}\n(run translate-all.sh and prepare-cache.py first, '
                  'or pass --failures/--report)', file=sys.stderr)
            return 2
    manifest, triage, warnings = analyze(failures, report, args.pack)
    print(manifest, end='')
    if args.manifest:
        args.manifest.write_text(manifest)
        print(f'manifest written: {args.manifest}')
    if args.json:
        structured = {
            'generated_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
            'failures_file': str(failures),
            'report_file': str(report),
            'warnings': warnings,
            'shaders': triage,
        }
        args.json.write_text(json.dumps(structured, indent=2) + '\n')
        print(f'structured triage written: {args.json}')
    if any(t['category'] == 'uncategorized' for t in triage):
        print('uncategorized failures remain: extend the category tables in '
              f'{pathlib.Path(__file__).name} and re-run', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
