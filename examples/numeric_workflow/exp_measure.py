"""Production exp workloads. Run after exp_benchmark check succeeds.

Usage: python3 exp_measure.py <binary> <output.csv> [cpu-id]
An optional Linux CPU id pins each process with taskset. One worker, cache off,
one warmup and seven measured runs; every timed output is checked afterwards.
"""
import subprocess
import sys


def main():
    binary, output = sys.argv[1:3]
    prefix = ['taskset', '-c', sys.argv[3]] if len(sys.argv)>3 else []
    with open(output, 'w') as log:
        log.write('backend,layer,N,range,repetitions,median_us,min_us,max_us,peak_payload_bytes,checksum\n')
        workloads = [(n, r) for n in (1, 65, 256, 16384, 262144, 4194304) for r in ('10', '80')]
        workloads += [(65, 'mixed'), (4097, 'mixed')]
        for n, span in workloads:
            for layer in ('public', 'core', 'raw'):
                if layer == 'raw' and (span == 'mixed' or n < 16384):
                    continue
                result = subprocess.run(prefix+[binary,layer,str(n),span,'7'],
                                        check=True, text=True,capture_output=True)
                log.write(result.stdout)
                log.flush()
                print(result.stdout, end='', flush=True)


if __name__ == '__main__':
    main()
