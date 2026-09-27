"""Independent integer-coordinate and raw-IEEE-bit NUM-09 workflow oracle."""
import itertools
import math
import random
import subprocess
import sys


def coordinates(shape):
    return itertools.product(*(range(n) for n in shape))


def flatten(coordinate, shape):
    index = 0
    for position, extent in zip(coordinate, shape):
        index = index*extent+position
    return index


def main():
    rng = random.Random(909)
    rows, expected = [], []

    def add(operation, dtype, shape, parameter, layout, values, starts=None, steps=None):
        row = f'{operation} {dtype} ' + ','.join(map(str,shape)) + ' ' + ','.join(map(str,parameter)) + ' ' + layout + ' '
        row += ' '.join(f'{b:x}' for b in values)
        if operation == 'reshape':
            want = values
        elif operation == 'transpose':
            output = [shape[j] for j in parameter]
            want = []
            for coordinate in coordinates(output):
                source = [0]*len(shape)
                for j,p in enumerate(parameter):
                    source[p] = coordinate[j]
                want.append(values[flatten(source,shape)])
        else:
            row += ' ' + ' '.join(map(str,starts+steps))
            valid = all(0 <= a < n and (c == 1 or (d != 0 and 0 <= a+(c-1)*d < n))
                        for a,d,c,n in zip(starts,steps,parameter,shape))
            want = [values[flatten([a+p*d if c>1 else a for a,p,d,c in zip(starts,coordinate,steps,parameter)],shape)]
                    for coordinate in coordinates(parameter)] if valid else None
        rows.append(row+'\n')
        expected.append('error' if want is None else ' '.join(f'{b:x}' for b in want))

    for dtype,width in [(1,8),(2,64),(3,64),(4,32)]:
        raw_special = [0,1,(1<<width)-1,1<<(width-1)]
        if dtype == 3:
            raw_special += [0x7ff0000000000001,0xfff8000000001234,0x7ff0000000000000]
        elif dtype == 4:
            raw_special += [0x7f800001,0xffc01234,0x7f800000]
        for shape in [[2,3],[3,2],[2,3,2],[1,2,3]]:
            values = [rng.choice(raw_special) if i%2 else rng.getrandbits(width) for i in range(math.prod(shape))]
            targets = [[math.prod(shape)],[1,math.prod(shape)],[3,math.prod(shape)//3]]
            for target in targets:
                for layout in ['view','auto','dense']:
                    add('reshape',dtype,shape,target,layout,values)
            for permutation in itertools.permutations(range(len(shape))):
                for layout in ['view','auto','dense']:
                    add('transpose',dtype,shape,list(permutation),layout,values)
        for _ in range(75):
            rank = rng.randrange(1,4)
            shape = [rng.randrange(1,5) for _ in range(rank)]
            count = [rng.randrange(1,n+1) for n in shape]
            starts,steps = [],[]
            for n,c in zip(shape,count):
                step = rng.choice([-2,-1,0,1,2])
                if c == 1:
                    step = rng.choice([0,-(1<<63),(1<<63)-1])
                starts.append(rng.randrange(-1,n+1))
                steps.append(step)
            values = [rng.choice(raw_special) if i%2 else rng.getrandbits(width) for i in range(math.prod(shape))]
            add('slice',dtype,shape,count,rng.choice(['view','auto','dense']),values,starts,steps)
    output = subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else 'strict','oracle'],
                            input=''.join(rows),text=True,capture_output=True,check=True)
    actual = [line.strip() for line in output.stdout.splitlines()]
    if len(actual)!=len(expected):
        raise AssertionError((len(actual),len(expected),output.stderr))
    for row,want,got in zip(rows,expected,actual):
        if want!=got:
            raise AssertionError((row,want,got))
    print(f'independent integer coordinate/raw-bit layout oracle: {len(expected)} cases passed')


if __name__ == '__main__':
    main()
