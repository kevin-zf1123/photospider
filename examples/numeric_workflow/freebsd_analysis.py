"""Extended numeric timing campaign. Run under cpuset on FreeBSD.

Usage: python3.12 freebsd_analysis.py CURRENT_TRIG BASELINE_TRIG EXP [OUTPUT]
Each case checks its output in the driver. Profiling is deliberately separate.
The CSV keeps one row per complete process (one warmup plus seven samples).
"""
import csv
import subprocess
import sys


def main():
    current, baseline, exp = sys.argv[1:4]
    stream = open(sys.argv[4], 'w', newline='') if len(sys.argv) > 4 else sys.stdout
    writer = csv.writer(stream)
    writer.writerow(['phase', 'round', 'backend', 'distribution', 'layout',
                     'function', 'layer', 'N', 'span', 'repetitions',
                     'median_us', 'min_us', 'max_us', 'peak_payload_bytes', 'checksum'])

    def trig(phase, name, layer, n, backend='current', distribution='normal', layout=3, run=0):
        span = '.25' if name in ('sinpi', 'cospi') else '1'
        executable = current if backend == 'current' else baseline
        command = [executable, name, layer, str(n), span, '7', 'measure',
                   distribution, str(layout)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=120, check=True)
        writer.writerow([phase, run, backend, distribution, layout] + next(csv.reader([result.stdout.strip()])))
        stream.flush()

    names = ('sin', 'cos', 'sinpi', 'cospi', 'sinc', 'sincpi')
    for name in names:
        for n in (1, 7, 8, 9, 63, 64, 65, 256, 4096, 65536, 262144, 1048576, 4194304):
            for layer in ('public', 'core', 'raw'):
                trig('scale', name, layer, n)
            if n in (1, 8, 64, 256, 4096, 65536):
                for layer in ('public', 'core'):
                    trig('scale', name, layer, n, 'baseline')
    # Alternating process order across three independent paired rounds.
    for run in range(3):
        order = ('baseline', 'current') if run % 2 == 0 else ('current', 'baseline')
        for name in names:
            for backend in order:
                trig('paired', name, 'public', 262144, backend, run=run)
    for name in names:
        for distribution in ('mixed', 'outside', 'landmark', 'tiny'):
            n = 1024 if distribution in ('outside', 'tiny') else 4096
            for backend in ('baseline', 'current'):
                for layer in ('public', 'core'):
                    trig('domain', name, layer, n, backend, distribution)
        for layout in (0, 1, 2, 3):
            for layer in (('public', 'core') if layout == 3 else ('core',)):
                trig('layout', name, layer, 65536, layout=layout)
    for n in (1, 7, 8, 9, 63, 64, 65, 4096, 65536, 262144, 1048576, 4194304):
        for span in ('10', '80'):
            for layer in ('public', 'core', 'raw'):
                result = subprocess.check_output([exp, layer, str(n), span, '7'], text=True, timeout=120)
                fields = next(csv.reader([result.strip()]))
                fields[0] = 'exp'
                writer.writerow(['exp', 0, 'current', 'normal', 3] + fields)
                stream.flush()
    for layer in ('public', 'core'):
        result = subprocess.check_output([exp, layer, '4097', 'mixed', '7'], text=True, timeout=120)
        fields = next(csv.reader([result.strip()]))
        fields[0] = 'exp'
        writer.writerow(['exp', 0, 'current', 'mixed', 3] + fields)
        stream.flush()
    if stream is not sys.stdout:
        stream.close()


if __name__ == '__main__':
    main()
