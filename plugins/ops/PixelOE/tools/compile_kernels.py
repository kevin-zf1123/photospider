"""AOT Slang CPU generation. Build products stay in the CMake binary tree."""
import argparse
import re
import subprocess
from pathlib import Path

ENTRIES = {
    'color/convert': ['luminance', 'lab_image'],
    'color/match': ['match_apply'],
    'color/blur': ['blur2d', 'blur_rows', 'blur_cols'] + [f'blur2d_sym_r{r}' for r in (2,4,8,16,32)],
    'color/blur_lowrank': ['lr_rows', 'lr_cols'] + [f'lr_{axis}_r{r}' for r in (2,4,8,16,32) for axis in ('rows','cols')],
    'outline/lattice': ['lattice_median', 'lattice_minmax'] + [f'lattice_stats_h{k}' for k in (2,4,6)],
    'outline/sliding': ['sliding_median', 'sliding_minmax_rows', 'sliding_minmax_cols'],
    'outline/weight': ['weight_raw', 'weight_dense', 'weight_gate'],
    'outline/morph': ['normalize_map', 'oe_blend', 'morph'],
    'downscale/contrast': ['contrast_downscale'] + [f'contrast_rgb_p{p}' for p in (2,3,4,5,6,8)],
    'downscale/kcentroid': ['kc_init', 'kc_iter', 'kc_final'],
    'reduce/reduce': ['seg_minmax_level', 'seg_sum_level', 'lab_moments_partial', 'moments_merge'],
    'reduce/stopdiff': ['diff_partial', 'diff_final'],
    'quant/weights': ['quant_weights'],
    'quant/kmeans': ['km_init', 'km_assign', 'km_partial', 'km_update', 'km_final'],
    'quant/repeat': ['rp_log', 'rp_shift_exp', 'rp_counts', 'rp_select_init', 'rp_hist', 'rp_select_step', 'rp_tie_count', 'rp_tie_scan', 'rp_mark'],
    'dither/dither': ['dither_ordered', 'ed_errors', 'ed_apply', 'palette_map'],
    'sharpen/sharpen': ['sharpen'],
    'resample/resample': ['pad_replicate', 'interpolate', 'csr_pass', 'upscale_nearest_exact'],
}

def generate(slangc, root, out):
    out.mkdir(parents=True, exist_ok=True)
    declarations, records = [], []
    for module, entries in ENTRIES.items():
        for entry in entries:
            dest = out / f'{entry}.cpp'
            cmd = [str(slangc), str(root / (module + '.slang')), '-entry', entry,
                   '-target', 'cpp', '-fp-mode', 'precise', '-o', str(dest)]
            for directory in [root, root / 'common', root / 'color']:
                cmd += ['-I', str(directory)]
            subprocess.run(cmd, check=True)
            text = dest.read_text()
            group = re.search(r'// \[numthreads\((\d+), (\d+), (\d+)\)\]\s*SLANG_PRELUDE_EXPORT\s*void ' + entry + r'\(', text)
            params = re.search(r'struct (EntryPointParams_\d+)\s*\{(.*?)\};', text, re.S)
            if not group or not params:
                raise RuntimeError(f'Unrecognized Slang CPU ABI for {entry}')
            assignments = []
            for line in params[2].strip().splitlines():
                ctype, name = line.strip().rstrip(';').rsplit(' ', 1)
                key = re.sub(r'_\d+$', '', name)
                buf = re.fullmatch(r'(?:RW)?StructuredBuffer<(\w+)>', ctype)
                if buf:
                    assignments.append(f'  p.{name} = {{static_cast<{buf[1]}*>(a.get("{key}").data), a.get("{key}").count}};')
                elif ctype in ['uint32_t', 'int32_t', 'float', 'uint64_t', 'int64_t']:
                    field = 'f' if ctype == 'float' else 'u'
                    assignments.append(f'  p.{name} = static_cast<{ctype}>(a.get("{key}").{field});')
                else:
                    raise RuntimeError(f'Unsupported parameter {ctype}')
            first, body = text.split('\n', 1)
            if not first.startswith('#include '):
                raise RuntimeError('Slang prelude include changed')
            namespace = 'px_generated_' + entry
            text = first + '\nnamespace ' + namespace + ' {\n' + body + '\n}\n'
            text += '\n#include "runtime.hpp"\n'
            text += f'void px_{entry}(const px::Range& range, const px::Arguments& a) {{\n  {namespace}::{params[1]} p{{}};\n'
            text += '\n'.join(assignments)
            text += f'\n  ComputeVaryingInput r{{{{range.begin[0], range.begin[1], range.begin[2] }}, {{range.end[0], range.end[1], range.end[2] }}}};\n  {namespace}::{entry}(&r, &p, nullptr);\n}}\n'
            dest.write_text(text)
            declarations.append(f'void px_{entry}(const px::Range&, const px::Arguments&);')
            records.append(f'  {{"{entry}", {{{", ".join(group.groups())}}}, px_{entry}}},')
    (out / 'kernels.cpp').write_text('#include "runtime.hpp"\n' + '\n'.join(declarations) + '\nnamespace px {\nconst std::vector<Kernel>& kernels() {\nstatic const std::vector<Kernel> k = {\n' + '\n'.join(records) + '\n}; return k; }\n}\n')

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--slangc', type=Path, required=True)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    generate(a.slangc, a.root, a.out)
