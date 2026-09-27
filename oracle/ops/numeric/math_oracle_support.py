"""Independent MPFR 4.2+ directed reference; never linked into the kernel.

The LP64 MPFR ABI is used only by the manual Python oracle on macOS and Linux.
Every temporary is cleared at scope exit. Production numeric operations use
host-accounted fixed limbs, not this reference library or its allocation hooks.
"""
import ctypes as C
import ctypes.util
import os
import platform
import struct


class MPFloat(C.Structure):
    _fields_ = [('precision', C.c_long), ('sign', C.c_int),
                ('exponent', C.c_long), ('limbs', C.POINTER(C.c_ulong))]


class MPFR:
    nearest, upward, downward = 0, 2, 3

    def __init__(self, precision):
        path = os.environ.get('PHOTOSPIDER_ORACLE_MPFR')
        if not path and platform.machine() == 'arm64' and os.path.exists('/opt/homebrew/opt/mpfr/lib/libmpfr.dylib'):
            path = '/opt/homebrew/opt/mpfr/lib/libmpfr.dylib'
        path = path or ctypes.util.find_library('mpfr')
        if not path or C.sizeof(C.c_long) != 8:
            raise RuntimeError('manual oracle requires MPFR 4.2+ on an LP64 host')
        self.lib = C.CDLL(path)
        self.lib.mpfr_get_version.restype = C.c_char_p
        self.version = self.lib.mpfr_get_version().decode()
        if tuple(map(int, self.version.split('.')[:2])) < (4, 2):
            raise RuntimeError('manual oracle requires MPFR 4.2+')
        pointer = C.POINTER(MPFloat)
        self.lib.mpfr_init2.argtypes = [pointer, C.c_long]
        self.lib.mpfr_clear.argtypes = [pointer]
        self.lib.mpfr_set_d.argtypes = [pointer, C.c_double, C.c_int]
        self.lib.mpfr_set_str.argtypes = [pointer, C.c_char_p, C.c_int, C.c_int]
        self.lib.mpfr_set.argtypes = [pointer, pointer, C.c_int]
        self.lib.mpfr_cmp.argtypes = [pointer, pointer]
        self.lib.mpfr_get_d.argtypes = [pointer, C.c_int]
        self.lib.mpfr_get_d.restype = C.c_double
        self.lib.mpfr_get_flt.argtypes = [pointer, C.c_int]
        self.lib.mpfr_get_flt.restype = C.c_float
        self.lib.mpfr_const_pi.argtypes = [pointer, C.c_int]
        for name in ('exp', 'log', 'sin', 'cos', 'tan', 'atan', 'sqrt',
                     'sinpi', 'cospi', 'tanpi', 'neg'):
            getattr(self.lib, 'mpfr_'+name).argtypes = [pointer, pointer, C.c_int]
        for name in ('add', 'sub', 'mul', 'div', 'pow', 'atan2', 'atan2pi'):
            getattr(self.lib, 'mpfr_'+name).argtypes = [pointer, pointer, pointer, C.c_int]
        self.precision = precision
        self.values = []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        for value in reversed(self.values):
            self.lib.mpfr_clear(C.byref(value))
        self.values.clear()

    def value(self):
        value = MPFloat()
        self.lib.mpfr_init2(C.byref(value), self.precision)
        self.values.append(value)
        return value

    def raw(self, bits, dtype):
        value = self.value()
        floating = struct.unpack('>f' if dtype == 4 else '>d', bits.to_bytes(4 if dtype == 4 else 8, 'big'))[0]
        self.lib.mpfr_set_d(C.byref(value), floating, self.nearest)
        return value

    def integer(self, integer):
        value = self.value()
        assert self.lib.mpfr_set_str(C.byref(value), str(integer).encode(), 10, self.nearest) == 0
        return value

    def pi(self, rounding):
        value = self.value()
        self.lib.mpfr_const_pi(C.byref(value), rounding)
        return value

    def unary(self, function, value, rounding):
        output = self.value()
        getattr(self.lib, 'mpfr_'+function)(C.byref(output), C.byref(value), rounding)
        return output

    def binary(self, function, a, b, rounding):
        output = self.value()
        getattr(self.lib, 'mpfr_'+function)(C.byref(output), C.byref(a), C.byref(b), rounding)
        return output

    def bits(self, value, dtype):
        converted = (self.lib.mpfr_get_flt if dtype == 4 else self.lib.mpfr_get_d)(C.byref(value), self.nearest)
        return int.from_bytes(struct.pack('>f' if dtype == 4 else '>d', converted), 'big')

    def compare(self, a, b):
        return self.lib.mpfr_cmp(C.byref(a), C.byref(b))


def direct(function, dtype, a, b=None):
    """Correct-rounding certificate from two independently directed MPFR calls."""
    for precision in (128, 256, 512, 1024, 2048, 4096, 8192):
        with MPFR(precision) as oracle:
            x = oracle.raw(a, dtype)
            if b is None:
                lower = oracle.unary(function, x, oracle.downward)
                upper = oracle.unary(function, x, oracle.upward)
            else:
                y = oracle.raw(b, dtype)
                lower = oracle.binary(function, x, y, oracle.downward)
                upper = oracle.binary(function, x, y, oracle.upward)
            lo, hi = oracle.bits(lower, dtype), oracle.bits(upper, dtype)
            if lo == hi:
                return lo
    raise ArithmeticError('independent MPFR bounds did not certify destination rounding')


def negate_bounds(oracle, pair):
    return (oracle.unary('neg', pair[1], oracle.downward),
            oracle.unary('neg', pair[0], oracle.upward))


def multiply_bounds(oracle, a, b, divide=False):
    function = 'div' if divide else 'mul'
    lower = [oracle.binary(function, x, y, oracle.downward) for x in a for y in b]
    upper = [oracle.binary(function, x, y, oracle.upward) for x in a for y in b]
    lo, hi = lower[0], upper[0]
    for value in lower[1:]:
        if oracle.compare(value, lo) < 0:
            lo = value
    for value in upper[1:]:
        if oracle.compare(value, hi) > 0:
            hi = value
    return lo, hi


def fraction_bounds(oracle, value):
    numerator = oracle.integer(value.numerator)
    denominator = oracle.integer(value.denominator)
    return (oracle.binary('div', numerator, denominator, oracle.downward),
            oracle.binary('div', numerator, denominator, oracle.upward))


def rational_pi(function, dtype, numerator, denominator):
    from fractions import Fraction
    from reduction_oracle import format_info
    fraction, bias, width = format_info(dtype)
    sign = 1 << (width-1)
    one = bias << fraction
    nan = (((1 << (width-fraction-1))-1) << fraction) | (1 << (fraction-1))
    ratio = Fraction(numerator, denominator)
    if not numerator:
        return one if function in ('cospi', 'sincpi') else 0
    if ratio.denominator == 1:
        if function == 'cospi':
            return one | (sign if ratio.numerator % 2 else 0)
        return 0 if function == 'sincpi' else sign if numerator < 0 else 0
    if ratio.denominator == 2:
        if function == 'cospi':
            return 0
        if function == 'tanpi':
            return nan
    absolute = abs(ratio)
    quarter = round(absolute*2)
    reduced = absolute-Fraction(quarter, 2)
    for precision in (128,256,512,1024,2048,4096,8192):
        with MPFR(precision) as oracle:
            low, high = fraction_bounds(oracle, reduced)
            sine = (oracle.unary('sinpi', low, oracle.downward),
                    oracle.unary('sinpi', high, oracle.upward))
            cosine = ((oracle.unary('cospi', high, oracle.downward),
                       oracle.unary('cospi', low, oracle.upward)) if reduced >= 0 else
                      (oracle.unary('cospi', low, oracle.downward),
                       oracle.unary('cospi', high, oracle.upward)))
            quadrant = quarter % 4
            sine, cosine = (cosine, sine) if quadrant % 2 else (sine, cosine)
            if quadrant >= 2:
                sine = negate_bounds(oracle, sine)
            if quadrant in (1, 2):
                cosine = negate_bounds(oracle, cosine)
            if function == 'sinpi':
                result = negate_bounds(oracle, sine) if numerator < 0 else sine
            elif function == 'cospi':
                result = cosine
            elif function == 'tanpi':
                result = multiply_bounds(oracle, sine, cosine, True)
                if numerator < 0:
                    result = negate_bounds(oracle, result)
            else:
                pi = oracle.pi(oracle.downward), oracle.pi(oracle.upward)
                full = fraction_bounds(oracle, absolute)
                argument = multiply_bounds(oracle, pi, full)
                result = multiply_bounds(oracle, sine, argument, True)
            lo, hi = (oracle.bits(value, dtype) for value in result)
            if lo == hi:
                return lo
    raise ArithmeticError('rational pi oracle did not resolve destination rounding')


def cardinal(function, dtype, raw):
    from reduction_oracle import format_info
    _, _, width = format_info(dtype)
    magnitude = raw & ((1 << (width-1))-1)
    for precision in (128,256,512,1024,2048,4096,8192):
        with MPFR(precision) as oracle:
            x = oracle.raw(magnitude, dtype)
            sine_function = 'sin' if function == 'sinc' else 'sinpi'
            sine = (oracle.unary(sine_function, x, oracle.downward),
                    oracle.unary(sine_function, x, oracle.upward))
            argument = (x, x)
            if function == 'sincpi':
                argument = multiply_bounds(oracle, argument,
                    (oracle.pi(oracle.downward), oracle.pi(oracle.upward)))
            result = multiply_bounds(oracle, sine, argument, True)
            lo, hi = (oracle.bits(value, dtype) for value in result)
            if lo == hi:
                return lo
    raise ArithmeticError('whole cardinal function oracle did not resolve rounding')
