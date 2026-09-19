"""Independent exact coordinates and stepwise Fraction/MPFR expression oracle."""
import ast
import json
import math
import random
import struct
import subprocess
import sys
from fractions import Fraction

from certified_oracle import reference as mathematical
from elementary_oracle import reference as elementary
from comparison_oracle import number
from sequence_oracle import ieee_round
from math_oracle_support import MPFR

SIGN = 1 << 63
INF = 0x7ff0000000000000
PI = 0x400921fb54442d18
E = 0x4005bf0a8b145769


def bits(value):
    return int.from_bytes(struct.pack('>d', value), 'big')


class NumericError(Exception):
    pass


def rounded(value, width=64, zero_negative=False):
    result = ieee_round(value, width, zero_negative)
    if result is None:
        raise NumericError('overflow')
    return result


def evaluate(source, x, coefficients):
    translated = source.replace('^', '**')
    tree = ast.parse(translated, mode='eval')
    def visit(node):
        if isinstance(node, ast.Constant):
            return rounded(Fraction(ast.get_source_segment(translated, node)))
        if isinstance(node, ast.Name):
            return {'x': x, 'pi': PI, 'e': E, **coefficients}[node.id]
        if isinstance(node, ast.UnaryOp):
            value = visit(node.operand)
            return value ^ SIGN if isinstance(node.op, ast.USub) else value
        if isinstance(node, ast.BinOp):
            a, b = visit(node.left), visit(node.right)
            operation = {ast.Add: 'add', ast.Sub: 'subtract', ast.Mult: 'multiply',
                         ast.Div: 'divide', ast.Pow: 'pow'}[type(node.op)]
        else:
            args = [visit(child) for child in node.args]
            a, b = args[0], args[1] if len(args) > 1 else 0
            operation = node.func.id
        u, v = number(a, 3), number(b, 3)
        if operation == 'divide' and not v:
            raise NumericError('divide')
        if (operation == 'sqrt' and u < 0) or (operation == 'ln' and u <= 0):
            raise NumericError('domain')
        if operation == 'pow':
            if not u and v < 0:
                raise NumericError('divide')
            if u < 0 and v.denominator != 1:
                raise NumericError('domain')
        basic = {'add': 8, 'subtract': 9, 'multiply': 10, 'divide': 11,
                 'abs': 0, 'sqrt': 2, 'min': 12, 'max': 13}
        functions = {'exp': 0, 'ln': 1, 'sin': 2, 'cos': 3, 'tan': 4, 'pow': 10}
        result = (elementary(basic[operation], 3, a, b) if operation in basic else
                  mathematical(functions[operation], 3, a, b))
        if (result & (SIGN-1)) >= INF:
            raise NumericError('overflow')
        return result
    return visit(tree.body)


def reference(row):
    source, count, dtype, inputs, names, selected = row
    def read(port):
        kind, raw = inputs[port]
        value = number(raw, kind)
        if value is None or value in (math.inf, -math.inf):
            raise NumericError('domain')
        return rounded(value, 64, bool(raw >> (31 if kind == 4 else 63)))
    start = read(0)
    end = read(1) if count > 1 else start
    a, b = number(start, 3), number(end, 3)
    if count > 1:
        if a == b:
            raise NumericError('domain')
        step = rounded((b-a)/(count-1))
        if not (step & (SIGN-1)):
            raise NumericError('overflow')
    else:
        step = 0
    axis = [start, end, step]
    if not selected:
        return [], axis
    coefficients = {name: read(i+2) for i, name in enumerate(names)}
    def coordinate(i):
        if i == 0:
            return start
        if i+1 == count:
            return end
        return rounded(((count-1-i)*a+i*b)/(count-1))
    result = []
    for i in selected:
        x = coordinate(i)
        for neighbor in (i-1, i+1):
            if 0 <= neighbor < count and number(x, 3) == number(coordinate(neighbor), 3):
                raise NumericError('overflow')
        value = evaluate(source, x, coefficients)
        if dtype == 4:
            value = rounded(number(value, 3), 32, bool(value & SIGN))
        result.append(value)
    return result, axis


def cases():
    rng = random.Random(1001)
    sources = ['2*x+1', 'x*x', 'sin(x)', 'cos(x)', 'tan(x)', 'exp(x)', 'ln(x)',
               'sqrt(x)', 'abs(x)', 'min(x,-x)', 'max(x,-x)', 'x^0', 'x^3',
               '1/(exp(x)-a)', '(a+b)-a', 'a*x+b', 'sin(x)^2+cos(x)^2',
               'exp(ln(x))', 'min(exp(1000),1)', '-2^2', '2^-2', '2^3^2',
               'pi', 'e', '0^0', '1/0', 'sqrt(-1)', 'ln(0)', '(-1)^.5',
               '.0000000000000001*x', '1e-9999', '-1e-9999', '1e40', '1e-45']
    for dtype in (3, 4):
        for source in sources:
            names = sorted({node.id for node in ast.walk(ast.parse(source.replace('^', '**'), mode='eval'))
                            if isinstance(node, ast.Name) and node.id in ('a', 'b')})
            for start, end, count in [(0, 1, 5), (1, 0, 5), (-1, 1, 3), (2, math.nan, 1)]:
                inputs = [(3, bits(start)), (3, bits(end))]
                inputs += [(4, 0x40000000) if name == 'a' else (3, bits(1)) for name in names]
                yield source, count, dtype, inputs, names, list(range(count))
        for _ in range(200):
            source = rng.choice(sources[:18])
            names = sorted({node.id for node in ast.walk(ast.parse(source.replace('^', '**'), mode='eval'))
                            if isinstance(node, ast.Name) and node.id in ('a', 'b')})
            count = rng.randrange(1, 9)
            start, end = [bits(rng.uniform(-5, 5)) for _ in range(2)]
            inputs = [(3, start), (3, end)] + [(3, bits(rng.uniform(-3, 3))) for _ in names]
            selected = sorted(rng.sample(range(count), rng.randrange(1, count+1)))
            yield source, count, dtype, inputs, names, selected
    for start, end, count in [(bits(1), bits(1)+1, 3), (INF-1, SIGN|(INF-1), 3),
                              (INF-1, SIGN|(INF-1), 2), (1, 2, 3),
                              (SIGN, 0, 2), (SIGN, bits(1), 2), (0, 1, 2)]:
        for selected in ([], [0], [1], list(range(count))):
            yield 'x', count, 3, [(3, start), (3, end)], [], selected
    for source in ('ln(x)', 'a*x+b', 'min(exp(1000),1)'):
        names = ['a', 'b'] if 'b' in source else []
        inputs = [(3, bits(0)), (3, bits(1))] + [(3, INF|0x42)]*len(names)
        yield source, 3, 3, inputs, names, []
    for dtype in (3, 4):
        exponential = mathematical(0, 3, bits(.5), 0)
        for delta in (-1, 0, 1):
            yield '1/(exp(x)-a)', 1, dtype, [(3, bits(.5)), (3, INF),
                                           (3, exponential+delta)], ['a'], [0]
        yield '(a+b)-a', 1, dtype, [(3, 0), (3, INF), (3, bits(2**53)),
                                   (4, 0x3f800000)], ['a', 'b'], [0]
        yield 'a1*x+_b2-Z9', 3, dtype, [(3, 0), (3, bits(1)), (3, bits(1)),
                                       (4, 0x3f000000), (3, bits(2))], ['Z9', '_b2', 'a1'], [0, 1, 2]
    # Whole-token decimal midpoint with a late nonzero digit.
    literal = '1.00000000000000011102230246251565404236316680908203125'
    for source in (literal, literal+'0'*3900+'1'):
        yield source, 1, 3, [(3, 0), (3, INF)], [], [0]


def main():
    rows, encoded, wanted = list(cases()), [], []
    for row in rows:
        source, count, dtype, inputs, names, selected = row
        encoded.append(f'{dtype} {count} {len(inputs)} '+' '.join(f'{kind} {raw:x}' for kind, raw in inputs)+' '+
                       json.dumps(source)+' '+json.dumps(' '.join(names))+' '+json.dumps(','.join(map(str, selected))))
        try:
            values, axis = reference(row)
            wanted.append(' '.join(f'{x:x}' for x in values)+' | '+' '.join(f'{x:x}' for x in axis))
        except NumericError as error:
            wanted.append('error '+str(error))
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    result = subprocess.run([sys.argv[1], profile, 'oracle'], input='\n'.join(encoded)+'\n',
                            text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(rows), (len(actual), len(rows), result.stderr)
    for i, (got, expected) in enumerate(zip(actual, wanted)):
        if expected.startswith('error'):
            assert got.startswith(expected+' '), (i, rows[i], got, expected)
        else:
            assert got.strip() == expected.strip(), (i, rows[i], got, expected)
    with MPFR(128) as oracle:
        version = oracle.version
    print(f'{len(rows)} independent coordinate/stepwise Fraction/MPFR-{version} expression cases passed ({profile})')


if __name__ == '__main__':
    main()
