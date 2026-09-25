"""Run comparable public/core timings plus production raw-kernel timings.

Usage: python3 trig_measure.py CURRENT [BASELINE]
Supply a baseline driver built from the preceding batch commit. Each process
uses one warmup/seven samples, seed 404, one public worker, no result cache.
Run only on an otherwise idle host; prefix the script with taskset on Linux.
"""
import subprocess
import sys


def main():
    current = sys.argv[1]
    baseline = sys.argv[2] if len(sys.argv) > 2 else None
    print('backend,function,layer,N,span,repetitions,median_us,min_us,max_us,'
          'peak_payload_bytes,checksum', flush=True)
    for name in ('sin', 'cos', 'sinpi', 'cospi', 'sinc', 'sincpi'):
        span = '.25' if name in ('sinpi', 'cospi') else '1'
        for n in (4096, 262144, 1048576):
            for layer in ('public', 'core'):
                for label, executable in [('baseline', baseline), ('polynomial', current)]:
                    if not executable:
                        continue
                    result = subprocess.check_output(
                        [executable, name, layer, str(n), span, '7'], text=True)
                    print(label+','+result.strip(), flush=True)
            result = subprocess.check_output(
                [current, name, 'raw', str(n), span, '7'], text=True)
            print('polynomial,'+result.strip(), flush=True)
    # Same sincpi full normalized domain on both backends, including side lobes.
    for label, executable in [('baseline', baseline), ('polynomial', current)]:
        if executable:
            result = subprocess.check_output(
                [executable, 'sincpi', 'public', '262144', '1024', '7'], text=True)
            print(label+','+result.strip(), flush=True)


if __name__ == '__main__':
    main()
