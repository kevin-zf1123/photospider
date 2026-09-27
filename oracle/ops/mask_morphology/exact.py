"""Independent finite IEEE rounding and exact quadratic arithmetic.

No production kernel helpers, NumPy arithmetic, Decimal heuristic or double-
rounded Float32 reference is used. Python floats are only stored IEEE carriers.
"""
from __future__ import annotations
from dataclasses import dataclass
from fractions import Fraction as Q
import math
import struct

class OracleError(ValueError):
    def __init__(self, message: str, code: str = "InvalidDomain"):
        super().__init__(message)
        self.code = code

def format_info(dtype: str) -> tuple[int, int, int, int]:
    if dtype == "float32":
        return 24, -126, 127, 32
    if dtype == "float64":
        return 53, -1022, 1023, 64
    raise OracleError(f"unsupported floating dtype: {dtype}", "TypeMismatch")

def bits(x: float, dtype: str = "float64") -> int:
    return int.from_bytes(struct.pack(">f" if dtype == "float32" else ">d", x), "big")

def from_bits(b: int, dtype: str = "float64") -> float:
    width = format_info(dtype)[3]
    if not isinstance(b, int) or not 0 <= b < 1 << width:
        raise OracleError("invalid bit pattern")
    return struct.unpack(">f" if width == 32 else ">d", b.to_bytes(width//8, "big"))[0]

def atom(x, dtype: str = "float64"):
    """Parse stored input; bits:XXXXXXXX preserves exact payload and signed zero."""
    if isinstance(x, bool):
        raise OracleError("Boolean is not a numeric parameter")
    if dtype == "uint8":
        if not isinstance(x, int) or not 0 <= x <= 255:
            raise OracleError("UInt8 sample must be an integer in [0,255]")
        return x
    format_info(dtype)
    if isinstance(x, str):
        if not x.startswith("bits:"):
            raise OracleError("use bits:HEX for bit-pattern input")
        raw = x[5:]
        if len(raw) != format_info(dtype)[3]//4:
            raise OracleError("bit-pattern width does not match dtype")
        return from_bits(int(raw, 16), dtype)
    if isinstance(x, Q) or isinstance(x, int):
        return rn(Q(x), dtype)
    if not isinstance(x, float):
        raise OracleError("expected stored numeric sample")
    try:
        return from_bits(bits(x, dtype), dtype)
    except OverflowError as error:
        raise OracleError("input does not fit storage dtype") from error

def finite(x, name: str = "value") -> Q:
    if isinstance(x, bool):
        raise OracleError(f"{name}: Bool not allowed")
    if isinstance(x, Q):
        return x
    if not isinstance(x, (int, float)) or not math.isfinite(x):
        raise OracleError(f"{name}: finite numeric value required")
    return Q(x)

def param(x, name="parameter", minimum=None, strict=False) -> Q:
    # Statics are actual Float64 values (integers accepted as convenient exact literals).
    v = finite(atom(x, "float64"), name)
    if minimum is not None and (v <= minimum if strict else v < minimum):
        raise OracleError(f"{name}: lower bound violated")
    return v

def integer(x, name="integer", minimum=0, maximum=(1 << 63)-1) -> int:
    if isinstance(x, bool) or not isinstance(x, int) or not minimum <= x <= maximum:
        raise OracleError(f"{name}: Int64 in [{minimum},{maximum}] required")
    return x

def div_round_even(n: int, d: int) -> int:
    a, rem = divmod(n, d)
    return a + (2*rem > d or (2*rem == d and a % 2 == 1))

def floor_log2(v: Q) -> int:
    if v <= 0:
        raise OracleError("log2 magnitude must be positive")
    e = v.numerator.bit_length() - v.denominator.bit_length()
    if e >= 0:
        if v.numerator < v.denominator << e:
            e -= 1
    elif v.numerator << -e < v.denominator:
        e -= 1
    return e

def pow2(e: int) -> Q:
    return Q(1 << e) if e >= 0 else Q(1, 1 << -e)

def rn(value, dtype: str = "float64") -> float:
    if isinstance(value, Quadratic):
        return value.round(dtype)
    v = Q(value)
    p, emin, emax, width = format_info(dtype)
    sign = int(v < 0)
    v = abs(v)
    if not v:
        return 0.0
    e = max(emin, floor_log2(v))
    shift = p-1-e
    n, d = v.numerator, v.denominator
    if shift >= 0:
        n <<= shift
    else:
        d <<= -shift
    m = div_round_even(n, d)
    if m >= 1 << p:
        m >>= 1
        e += 1
    if e > emax:
        raise OracleError("finite result rounds to infinity", "ArithmeticOverflow")
    if m >= 1 << (p-1):
        exponent = e - emin + 1
        fraction = m - (1 << (p-1))
    else:
        exponent, fraction = 0, m
    return from_bits((sign << (width-1)) | (exponent << (p-1)) | fraction, dtype)

def sign_of(a: Q, b: Q, radicand: Q) -> int:
    """Sign of a+b*sqrt(radicand), exactly, without approximating sqrt."""
    if not b or not radicand:
        return (a > 0) - (a < 0)
    if not a:
        return (b > 0) - (b < 0)
    if (a > 0) == (b > 0):
        return 1 if a > 0 else -1
    d = a*a-b*b*radicand
    return ((d > 0)-(d < 0)) * (1 if a > 0 else -1)

@dataclass(frozen=True)
class Quadratic:
    """a+b*sqrt(q); sufficient for exact linear/cubic distance ramps."""
    a: Q
    b: Q
    q: Q

    def __post_init__(self):
        object.__setattr__(self, "a", Q(self.a))
        object.__setattr__(self, "b", Q(self.b))
        object.__setattr__(self, "q", Q(self.q))
        if self.q < 0:
            raise OracleError("negative radicand")

    def __add__(self, other):
        if isinstance(other, Quadratic):
            if self.q != other.q:
                raise OracleError("oracle supports a single quadratic extension")
            return Quadratic(self.a+other.a, self.b+other.b, self.q)
        return Quadratic(self.a+Q(other), self.b, self.q)
    __radd__ = __add__

    def __neg__(self):
        return Quadratic(-self.a, -self.b, self.q)
    def __sub__(self, other):
        return self + (-other)
    def __rsub__(self, other):
        return -self + other
    def __mul__(self, other):
        if isinstance(other, Quadratic):
            if self.q != other.q:
                raise OracleError("oracle supports a single quadratic extension")
            return Quadratic(self.a*other.a+self.b*other.b*self.q,
                             self.a*other.b+self.b*other.a, self.q)
        return Quadratic(self.a*Q(other), self.b*Q(other), self.q)
    __rmul__ = __mul__
    def __truediv__(self, other):
        v = Q(other)
        if not v:
            raise OracleError("zero denominator")
        return Quadratic(self.a/v, self.b/v, self.q)
    def compare(self, rational=0) -> int:
        return sign_of(self.a-Q(rational), self.b, self.q)

    def round(self, dtype="float64") -> float:
        if not self.b:
            return rn(self.a, dtype)
        sqn, sqd = math.isqrt(self.q.numerator), math.isqrt(self.q.denominator)
        if sqn*sqn == self.q.numerator and sqd*sqd == self.q.denominator:
            return rn(self.a+self.b*Q(sqn, sqd), dtype)
        s = self.compare()
        if not s:
            return 0.0
        v = self if s > 0 else -self
        p, emin, emax, width = format_info(dtype)
        maxbits = bits(float.fromhex('0x1.fffffep127') if dtype == 'float32'
                       else float.fromhex('0x1.fffffffffffffp1023'), dtype)
        lo, hi = 0, maxbits+1
        while hi-lo > 1:
            mid = (lo+hi)//2
            if v.compare(Q(from_bits(mid, dtype))) >= 0:
                lo = mid
            else:
                hi = mid
        lv = Q(from_bits(lo, dtype))
        hv = pow2(emax+1) if hi == maxbits+1 else Q(from_bits(hi, dtype))
        cmp = v.compare((lv+hv)/2)
        out = lo if cmp < 0 or (cmp == 0 and lo % 2 == 0) else hi
        if out == maxbits+1:
            raise OracleError("finite result rounds to infinity", "ArithmeticOverflow")
        return from_bits(out | ((s < 0) << (width-1)), dtype)

def sqrt_exact(q: Q) -> Quadratic:
    return Quadratic(Q(0), Q(1), Q(q))

def clip01(v: Q) -> Q:
    return max(Q(0), min(Q(1), v))

def curve01(v, curve: str):
    if curve not in ("linear", "smoothstep"):
        raise OracleError("curve must be linear or smoothstep")
    return v if curve == "linear" else v*v*(3-2*v)

def accelerated_ok(candidate: float, reference: float, dtype: str,
                   exact: bool = False) -> bool:
    """NUM final-output acceptance; callers mark discrete/copy results exact."""
    format_info(dtype)
    try:
        cb, rb = bits(candidate, dtype), bits(reference, dtype)
    except (OverflowError, TypeError):
        return False
    if exact or not math.isfinite(candidate) or not math.isfinite(reference):
        return cb == rb
    if reference == 0 or candidate == 0:
        return cb == rb
    if dtype == "float32":
        def ordered(b):
            return (~b & 0xffffffff) if b >> 31 else b | 0x80000000
        return abs(ordered(cb)-ordered(rb)) <= 4
    mag = abs(Q(reference))
    if mag < pow2(-126) or mag > Q(float.fromhex('0x1.fffffep127')):
        return cb == rb
    bound = 4*pow2(floor_log2(mag)-23)
    return abs(Q(candidate)-Q(reference)) <= bound
