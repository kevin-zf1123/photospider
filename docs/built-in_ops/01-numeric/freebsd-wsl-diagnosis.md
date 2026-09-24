# FreeBSD versus WSL: hybrid CPU HWP diagnosis, 2026-09-25

Follow-up: the user subsequently configured per-core HWP control in the loader
and rebooted. The [adapter optimization measurements](adapter-performance.md)
verify `machdep.hwpstate_pkg_ctrl=0` and a 4.737 GHz sustained CPU-2 load. The
restoration descriptions below record the state at the end of this earlier
diagnostic experiment, not the current machine configuration.

## Result

The dominant cause of the earlier FreeBSD slowdown is the active Intel HWP
package performance limit on this i9-12900. It is reproducible without changing
Photospider, compiler, libraries, affinity, inputs or the EPP percentage.

Writing the same EPP=50 through an E-core writes its cached maximum performance
value 38 into the package request. CPU 2, a P-core capable of performance value
64, then runs this workload at approximately 2.922 GHz. Writing EPP=50 through
CPU 0, a P-core, changes the shared maximum to 64; the same workload runs at
4.737 GHz. Alternating these writes reproduces both performance levels.
These HWP performance values are hardware scale values, not GHz or percentages.

The upstream FreeBSD [D57377 review](https://reviews.freebsd.org/D57377) and
[7b26353a59d6 commit](https://reviews.freebsd.org/rG7b26353a59d66dc1bc611fd042a49b9e3bd13699)
describe this same hybrid-core/package-control failure and disable package
control on hybrid systems. This host's active behavior still exposes the issue.
The conclusion concerns this installed kernel/configuration, not every FreeBSD
release or a general Linux-versus-FreeBSD performance ranking.

## Host and comparison boundaries

The running kernel, installed kernel and userland report 15.1-RELEASE; the
running kernel identifies itself as
`releng/15.1-n283562-96841ea08dcf GENERIC`. The observed settings are:

- `machdep.hwpstate_pkg_ctrl=1`.
- CPU 0/2 are P-cores: highest HWP performance 64.
- CPU 16/23 are E-cores: highest HWP performance 38.
- All measured benchmark processes are pinned to CPU 2. Its topology pairs it
  with CPU 3 as an SMT sibling. They are not E-cores.
- Explicit Clang 22.1.7 and isolated libc++/libc++abi/libunwind 22.1.7 remain
  unchanged throughout all new measurements.

The WSL reference is the retained `trig-time-linux.csv` from the earlier
Clang 18.1.3 run on an i9-12900. Its input checksums match the FreeBSD records.
WSL was not rerun in this investigation: the current `winpc` and `freebsdpc`
SSH aliases point to the same address, and the Windows host-key identity does
not match the active FreeBSD endpoint. No host-key checks were bypassed and no
host was rebooted. Residual cross-platform differences therefore remain
confounded by compiler/runtime versions and the historical WSL run conditions.

## The experiment that isolated the cause

First, a sustained existing binary run at the initial state reported 2.922 GHz.
The existing historical FreeBSD results also had that observed load frequency.
An initial EPP experiment unexpectedly changed performance when writing **50
back to 50 on CPU 0**, before its EPP=0 phase. All three phases, P-core write
50, write 0, write 50, ran at 4.737 GHz with essentially identical large-array
latencies. Thus it would be incorrect to attribute the gain to EPP=0.

The corrected experiment held EPP=50 throughout and varied only the device used
to make the write. Register-derived debug output was captured before each
measurement phase. The frequency probe and hardware counter workload ran
separately from ordinary latency measurements.

| Phase | Same-value write | Requested package maximum seen by CPU 2 | Load frequency |
| --- | --- | --- | --- |
| A | `dev.hwpstate_intel.23.epp=50` | 38 | 2922 MHz |
| B | `dev.hwpstate_intel.0.epp=50` | 64 | 4737 MHz |
| A repeat | `dev.hwpstate_intel.23.epp=50` | 38 | 2922 MHz |

Each phase uses the same existing binaries, one warmup, seven timed samples,
full output checks outside timing, one worker, disabled result cache, and no
optional managed ledger. Each has 25 timing rows: six current trig functions
at public/core/raw boundaries, small sin public/core, current exp at all three
boundaries, and baseline sin/sincpi public. Together with the first three
phases, this investigation adds 150 timing rows. No builds or other benchmark
campaigns ran concurrently.

Selected public medians in milliseconds, N=1,048,576:

| Workload | A: cap 38 | B: cap 64 | A repeat: cap 38 |
| --- | --- | --- | --- |
| Current sin | 8.027 | 4.980 | 8.054 |
| Current cos | 8.044 | 4.987 | 8.020 |
| Current sinpi | 7.995 | 4.975 | 8.017 |
| Current cospi | 8.000 | 4.968 | 7.956 |
| Current sinc | 7.984 | 4.993 | 7.988 |
| Current sincpi | 9.650 | 6.063 | 9.644 |
| Current exp, span 10 | 6.700 | 4.203 | 6.696 |
| Baseline sin | 121.575 | 74.372 | 121.158 |
| Baseline sincpi | 513.113 | 315.860 | 512.889 |

The original low-frequency register state was not captured before the first
sysctl write. Its exact raw initial MSR bits and boot write order are therefore
not claimed as observed. The same low frequency and latency are reproduced by
the explicitly observed cap-38 state, and the driver source provides the
mechanism. The boot-order origin is supported by that source and the upstream
fix rather than a captured boot trace.

## Instructions and cycles distinguish clock rate from software work

Separate `pmcstat -C -w 60` recordings count `inst_retired.any` and
`cpu_clk_unhalted.thread` for current sin core, N=1,048,576, 500 repetitions.
Counts cover the entire process including initialization and one warmup check;
latency is the driver's per-iteration median.

| Phase | Median us | Retired instructions | Unhalted cycles |
| --- | --- | --- | --- |
| A: cap 38 | 7528.18 | 60,606,469,153 | 11,009,318,544 |
| B: cap 64 | 4652.38 | 60,604,393,668 | 11,022,485,640 |
| A repeat: cap 38 | 7543.84 | 60,606,461,781 | 11,022,550,288 |

Instruction counts differ by less than 0.004% and cycle counts by about 0.12%.
Median time changes by approximately 1.62x, matching the 4737/2922 frequency
ratio. This directly supports a frequency-limit cause instead of additional
software instructions, missing AVX2, altered polynomial work or a new cache-miss
path explaining the large original gap.

## Comparison after removing the low package cap

The FreeBSD cap-64 column below takes the median of three EPP=50 process
medians: the initial P-core write, its EPP-restored phase, and the controlled
cap-64 phase. WSL numbers retain their historical single-sequence limitations.
All values are public milliseconds at N=1,048,576.

| Function | Historical WSL | Earlier FreeBSD | FreeBSD cap 64 | Cap-64 FreeBSD versus WSL |
| --- | --- | --- | --- | --- |
| sin | 4.752 | 8.015 | 4.980 | +4.8% |
| cos | 4.814 | 8.071 | 4.988 | +3.6% |
| sinpi | 4.487 | 8.033 | 4.975 | +10.9% |
| cospi | 4.613 | 8.004 | 4.962 | +7.6% |
| sinc | 4.419 | 7.986 | 4.966 | +12.4% |
| sincpi | 6.192 | 9.713 | 6.021 | -2.8% |

Most of the previously observed 57–81% excess public latency disappears.
Sincpi becomes slightly faster than its historical WSL median. The remaining
3.6–12.4% for the other functions cannot be assigned to the OS from this
experiment. Compiler 18 versus 22, C++/C runtime implementation, allocation and
historical WSL scheduling conditions are candidates requiring matched reruns.
The observed remaining gap does not justify claiming Clang 22 or libc++ is
slower; neither variable was isolated.

Small runs and raw kernels retain timing/cache variance. For example, the
controlled cap-64 N=1 public sin median was 40.1 us, while the three earlier
high-frequency phases were approximately 31.6–32.3 us. Do not infer a precise
small-array OS penalty from these samples. The large-array counter result is
the stronger causal evidence.

## Source mechanism and operational consequences

The captured host source `sys/x86/cpufreq/hwpstate_intel.c` shows:

1. `set_autonomous_hwp` reads each CPU's HWP capabilities and sets cached
   `sc->req.Maximum_Performance` from that CPU's `sc->high`.
2. With package control enabled, it writes this request to the shared
   `MSR_IA32_HWP_REQUEST_PKG` during each device's initialization.
3. `sysctl_epp_select` returns EPP from the per-device cached request. On a write
   it updates EPP, then writes the **whole cached request**, including its
   maximum performance, to the package MSR.

Consequently, reading EPP=50 does not reveal the effective maximum, and writing
that same value is not a no-op. A loop that writes all CPUs can leave an E-core's
limit as the final package request. The debug HWP request fields and actual load
frequency must be checked, not merely the EPP percentage.

For this host, a temporary P-core write `sysctl dev.hwpstate_intel.0.epp=50`
was experimentally sufficient to select cap 64. It is not a permanent driver
fix; a later E-core write or device initialization can replace it.
The durable configuration direction is per-core control via the loader tunable
`machdep.hwpstate_pkg_ctrl="0"`, or a kernel carrying the applicable hybrid-CPU
fix. The [FreeBSD hwpstate_intel manual](https://man.freebsd.org/cgi/man.cgi?query=hwpstate_intel&sektion=4&manpath=FreeBSD+15.1-RELEASE+and+Ports.quarterly)
documents package/per-core selection and EPP semantics. A loader change requires
a planned reboot and verification; no persistent configuration, kernel update
or reboot was performed here.

The final experiment restored EPP=50 and the reproducible original slow
cap-38 state. Final debug output confirms requested maximum 38. The earlier
performance reports remain valid measurements of that limited state, but must
not be used as unconstrained FreeBSD-versus-WSL performance results.

## Artifacts and correctness

Remote artifacts: `/home/alex/photospider-trig-20260925/diagnosis/`.
Local copies: ignored `build/num04-exp/freebsd/diagnosis/`.

- `epp_measure.py`, `epp_experiment.sh`, `package_experiment.sh`: executed commands;
  sudo was entered by the user in independent Terminal.app windows, with
  benchmarks dropped back to the ordinary `alex` account.
- `epp-*.csv`: all 150 rows; output checks and stored checksums pass.
- `state-cap*.txt`, `frequency-cap*.txt`, `counter-cap*.log`: direct register,
  frequency and hardware-count evidence.
- `epp-experiment.log`, `package-experiment.log`: setting transitions and restore.
- `host-final.txt`, `host-hwpstate_intel.c`, `host-hwpstate_common.c`,
  `link-command.txt`, `ldd.txt`: host, source and build/runtime context.

No production code or arithmetic expression changed. The previously passed
MPFR/CRV correctness corpus remains applicable; every new timing case also
completed the driver's output checks. No new full CTest or release matrix was
needed for this environment diagnosis.
