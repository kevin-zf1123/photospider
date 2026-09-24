"""Plot the extended campaign; requires matplotlib. Arguments: DATA_DIR OUTPUT."""
import csv
from pathlib import Path
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def rows(path):
    with open(path) as stream:
        return list(csv.DictReader(stream))


def main():
    root, output = Path(sys.argv[1]), Path(sys.argv[2])
    data = rows(root / 'timing-final.csv')
    fig, axes = plt.subplots(2, 2, figsize=(12, 8.4), layout='constrained')
    colors = {'public': '#7342a0', 'core': '#0076b8', 'raw': '#00876c'}
    for ax, name in zip(axes[0], ('sin', 'sincpi')):
        for layer, color in colors.items():
            values = [r for r in data if r['phase'] == 'scale' and
                      r['function'] == name and r['backend'] == 'current' and r['layer'] == layer]
            ax.loglog([int(r['N']) for r in values], [float(r['median_us']) for r in values],
                      '.-', label='current '+layer, color=color)
        values = [r for r in data if r['phase'] == 'scale' and
                  r['function'] == name and r['backend'] == 'baseline' and r['layer'] == 'public']
        ax.loglog([int(r['N']) for r in values], [float(r['median_us']) for r in values],
                  '--', color='#666666', label='baseline public')
        ax.set(title=name+': scaling', xlabel='Input elements', ylabel='Median latency (microseconds)')
        ax.grid(alpha=.2, which='both')
        ax.legend(fontsize=8)
    ax = axes[1, 0]
    names = ['sin', 'cos', 'sinpi', 'cospi', 'sinc', 'sincpi']
    for shift, distribution, color in [(-.19, 'normal', '#0076b8'), (.19, 'mixed', '#d95d39')]:
        values = []
        for name in names:
            r = next(r for r in data if r['function'] == name and r['backend'] == 'current' and
                     r['N'] == '4096' and r['layer'] == 'public' and
                     r['phase'] == ('scale' if distribution == 'normal' else 'domain') and
                     r['distribution'] == distribution)
            values.append(float(r['median_us'])/1000)
        ax.bar([i+shift for i in range(len(names))], values, .38, color=color,
               label='ordinary' if distribution == 'normal' else '1/16 changed lanes')
    ax.set_xticks(range(len(names)), names)
    ax.set_yscale('log')
    ax.set(title='Input-domain sensitivity: N=4,096', ylabel='Public median (milliseconds)')
    ax.grid(axis='y', alpha=.2)
    ax.legend(fontsize=8)
    ax = axes[1, 1]
    labels = ['inverse\nlinear F32', 'inverse\nPCHIP F32', 'lowpass\nHann F64', 'lowpass\nGaussian F64']
    for shift, profile, color in [(-.19, 'strict', '#666666'), (.19, 'x86', '#00876c')]:
        inv = rows(root / ('inverse-'+profile+'-float32.csv'))
        low = rows(root / ('lowpass-'+profile+'.csv'))
        values = [float(next(r['median_us'] for r in dataset if key in r['operation'] and r['requested'] == '256'))/1000
                  for dataset, key in [(inv, 'invert_linear'), (inv, 'invert_pchip'), (low, 'hann_sinc'), (low, 'gaussian')]]
        ax.bar([i+shift for i in range(4)], values, .38, color=color, label=profile)
    ax.set_xticks(range(4), labels)
    ax.set_yscale('log')
    ax.set(title='CRV: 256 observed outputs, managed ledger enabled', ylabel='Public median (milliseconds)')
    ax.grid(axis='y', alpha=.2)
    ax.legend(fontsize=8)
    fig.suptitle('FreeBSD 15.1 / i9-12900 / Clang 22.1.7 — measured performance', fontsize=14)
    fig.savefig(output, dpi=160)
    fig.savefig(output.with_suffix('.svg'))


if __name__ == '__main__':
    main()
