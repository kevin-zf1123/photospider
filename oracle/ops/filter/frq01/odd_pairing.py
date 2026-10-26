"""Exact coefficient proof for paired odd DFT codelets, independent of Ball.

The input scalars are formal variables. Nontrivial paired coefficients are
reduced over Q modulo the cyclotomic polynomial, without sampling inputs.
DC is v[0] + sum(v[j] + v[r-j]), so every DC input coefficient is exactly 1.
Every output retains v[0] with coefficient 1; imaginary input coefficients
multiply these by i. The remaining coefficients are checked below for both
DFT signs. Numerical rounding remains dft2's concern.
"""
from fractions import Fraction as Q

from oracle.ops.filter.common.spectral import reduce_poly


def verify_odd_pairing(radix):
    """Prove nontrivial paired coefficients for either DFT sign, odd r <= 31."""
    if radix < 3 or radix > 31 or radix % 2 == 0:
        raise ValueError("odd radix in [3, 31] required by this tiny proof")
    order = 4 * radix
    checked = 0
    for sign in (-1, 1):
        for output in range(1, radix // 2 + 1):
            for j in range(1, radix // 2 + 1):
                phase = sign * 4 * j * output % order
                cosine = [Q(0)] * order
                i_sine = [Q(0)] * order
                cosine[phase] += Q(1, 2)
                cosine[-phase % order] += Q(1, 2)
                i_sine[phase] += Q(1, 2)
                i_sine[-phase % order] -= Q(1, 2)
                for paired_output in (1, -1):
                    for paired_input in (1, -1):
                        # S*cos + i*D*sin, with D reversing the partner sign.
                        factor = paired_output * paired_input
                        coefficient = [c + factor * s
                                       for c, s in zip(cosine, i_sine)]
                        exponent = paired_output * paired_input * phase % order
                        for imaginary_input in (False, True):
                            shift = radix if imaginary_input else 0
                            difference = [Q(0)] * order
                            for k, value in enumerate(coefficient):
                                difference[(k + shift) % order] += value
                            difference[(exponent + shift) % order] -= 1
                            if reduce_poly(difference, order):
                                raise AssertionError((radix, sign, output, j,
                                                      paired_output, paired_input,
                                                      imaginary_input))
                            checked += 1
    return checked


if __name__ == "__main__":
    total = sum(verify_odd_pairing(r) for r in (7, 11, 13, 17, 19, 23, 29, 31))
    print(f"PASS {total} exact nontrivial paired complex DFT coefficients")
