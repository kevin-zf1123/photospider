import math
import random
import struct
import unittest
from fractions import Fraction as F
import exact as e


class ExactArithmeticTests(unittest.TestCase):
    def test_ieee_known_answers(self):
        cases=[(0,0,0),(1,0x3f800000,0x3ff0000000000000),(F(1,2),0x3f000000,0x3fe0000000000000),(-1,0xbf800000,0xbff0000000000000),(F(1,10),0x3dcccccd,0x3fb999999999999a)]
        for q,b32,b64 in cases:
            with self.subTest(q=q):
                self.assertEqual(e.round_bits(q,'float32'),b32);self.assertEqual(e.round_bits(q),b64)
    def test_ties_even(self):
        for dtype,p in [('float32',24),('float64',53)]:
            ulp=e.power2(-(p-1))
            self.assertEqual(e.round_fraction(1+ulp/2,dtype),1)
            self.assertEqual(e.round_fraction(1+3*ulp/2,dtype),e.round_fraction(1+2*ulp,dtype))
            self.assertEqual(e.round_fraction(-1-ulp/2,dtype),-1)
    def test_subnormal_signed_zero(self):
        for dtype,emin,width in [('float32',-149,32),('float64',-1074,64)]:
            z=e.power2(emin)
            self.assertEqual(e.round_bits(z,dtype),1)
            self.assertEqual(e.round_bits(z/2,dtype),0)
            self.assertEqual(e.round_bits(-z/2,dtype),1<<(width-1))
            self.assertEqual(e.round_bits(3*z/2,dtype),2)
            self.assertEqual(e.round_bits(0,dtype,True),1<<(width-1))
    def test_maxfinite_overflow(self):
        for dtype,p,emax in [('float32',24,127),('float64',53,1023)]:
            maximum=(2-e.power2(-(p-1)))*e.power2(emax)
            self.assertTrue(math.isfinite(e.round_fraction(maximum,dtype)))
            with self.assertRaises(OverflowError):e.round_fraction(2*e.power2(emax),dtype)
    def test_roundtrip_random_finite_bits(self):
        rng=random.Random(1729)
        for dtype,width,emask in [('float32',32,0x7f800000),('float64',64,0x7ff0000000000000)]:
            for _ in range(400):
                b=rng.getrandbits(width)
                if b&emask==emask:continue
                x=e.from_bits(b,dtype)
                self.assertEqual(e.round_bits(F(x),dtype,x==0 and bool(b>>(width-1))),b)
    def test_rational_round_against_python_f64(self):
        rng=random.Random(2026)
        for _ in range(300):
            q=F(rng.randrange(-10**30,10**30),rng.randrange(1,10**25))
            self.assertEqual(e.float_bits(float(q)),e.round_bits(q))
    def test_sqrt_known(self):
        self.assertEqual(e.float_bits(e.sqrt_fraction(2)),0x3ff6a09e667f3bcd)
        self.assertEqual(e.float_bits(e.sqrt_fraction(2,'float32'),'float32'),0x3fb504f3)
        self.assertEqual(e.sqrt_fraction(F(9,16)),.75)
    def test_sqrt_midpoint(self):
        for dtype,p in [('float32',24),('float64',53)]:
            step=e.power2(-(p-1));mid=1+step/2
            self.assertEqual(e.sqrt_fraction(mid*mid,dtype),1)
            nextmid=1+3*step/2
            self.assertEqual(e.sqrt_fraction(nextmid*nextmid,dtype),e.round_fraction(1+2*step,dtype))
    def test_sqrt_enclosures(self):
        for q in (F(0),F(2),F(3,7),e.power2(-2100),e.power2(2000)*3):
            lo,hi=e.sqrt_bounds(q,120)
            self.assertLessEqual(lo*lo,q);self.assertGreaterEqual(hi*hi,q)
            self.assertLessEqual(lo,hi)
    def test_sqrt_plus_cancellation(self):
        self.assertEqual(e.round_sqrt_plus(4,-2),0)
        self.assertEqual(e.round_sqrt_plus(2,-1),float.fromhex('0x1.a827999fcef32p-2'))
    def test_final_fp64_fp32_scale_budget(self):
        step=2**-23
        self.assertTrue(e.accelerated_ok(1.,1.+4*step))
        self.assertFalse(e.accelerated_ok(1.,1.+5*step))
        self.assertTrue(e.accelerated_ok(1.,math.nextafter(1.,2.)))
        self.assertFalse(e.accelerated_ok(0.,-0.))
    def test_acceleration_fallback_ranges(self):
        for r in (2**-127,float.fromhex('0x1p-1074'),float.fromhex('0x1p128')):
            self.assertTrue(e.accelerated_ok(r,r))
            self.assertFalse(e.accelerated_ok(r,math.nextafter(r,math.inf)))
        self.assertFalse(e.accelerated_ok(1.,math.inf))
    def test_acceleration_fp32_steps(self):
        for delta in range(7):
            self.assertEqual(e.accelerated_ok(1.,e.from_bits(0x3f800000+delta,'float32'),'float32'),delta<=4)
    def test_constant_payloads(self):
        bits=[0x80000000,0x7fc01234,0x7f800000,0x3f800000]
        out=e.constant_bits(bits,2,3)
        self.assertEqual(out[1][2],bits)
        out[0][0][0]=9;self.assertEqual(out[1][1][0],0x80000000)
    def test_coordinate_canvas_and_origin(self):
        self.assertEqual(e.coordinate(1,2,4,8,(10,-4)),(F(23,2),F(-3,2)))
        self.assertEqual(e.coordinate(1,2,4,8,(10,-4),True),(F(3,8),F(5,16)))
        for x in range(5):self.assertEqual(e.coordinate(x,0,5,1)[0],e.coordinate(x,0,10,1)[0])
    def test_rectangle_area(self):
        self.assertEqual(e.rectangle_coverage([F(1,4),F(1,4),F(3,4),F(3,4)]),F(1,4))
        self.assertEqual(e.rectangle_coverage([0,0,0,1]),0)
        self.assertEqual(e.rectangle_coverage([-1,-1,2,2]),1)
    def test_rectangle_disk_distance(self):
        self.assertEqual(e.rectangle_sdf((0,0),(-1,-1,1,1)),-1)
        self.assertEqual(e.rectangle_sdf((2,2),(-1,-1,1,1)),e.sqrt_fraction(2))
        self.assertEqual(e.disk_sdf((0,0),(0,0),2),-2)
        self.assertEqual(e.disk_sdf((3,4),(0,0),2),3)
    def test_checker_negative_floor(self):
        self.assertEqual(e.checker((F(-1,2),F(1,2)),(1,1)),1)
        self.assertEqual(e.checker((F(1,2),F(1,2)),(1,1)),0)
    def test_grid_degenerate_width(self):
        self.assertEqual(e.grid_pattern((F(1,4),F(1,4)),(2,2),(0,0)),0)
        self.assertEqual(e.grid_pattern((F(1,4),F(1,4)),(2,2),(2,0)),1)
    def test_ramp_impulse_bars(self):
        self.assertEqual([e.ramp(i,3) for i in range(3)],[0,F(1,2),1])
        self.assertEqual(e.ramp(0,1),0)
        impulse=e.impulse((3,3),(1,1));self.assertEqual(sum(impulse((x,y)) for x in range(3) for y in range(3)),1)
        self.assertEqual([e.color_bar(x,8) for x in range(8)],list(e.BARS))
        self.assertEqual(e.color_bar(0,1),e.BARS[4])
    def test_gradient_coordinates(self):
        self.assertEqual(e.linear_coordinate((F(1,2),0),(0,0),(2,0)),F(1,4))
        self.assertEqual(e.radial_coordinate((3,4),(0,0),(1,1)),5)
        self.assertEqual(e.norm_coordinate((1,1),(0,0),(2,2),'l1'),1)
        self.assertEqual(e.norm_coordinate((1,1),(0,0),(2,2),'linf'),F(1,2))
    def test_two_circle_coefficients(self):
        self.assertEqual(e.quadratic_coefficients((1,0),(0,0),0,(0,0),2),(-4,0,1))
    def test_spread(self):
        xs=[F(-1,4),0,1,F(5,4)]
        self.assertEqual([e.spread(x,'repeat') for x in xs],[F(3,4),0,0,F(1,4)])
        self.assertEqual([e.spread(x,'reflect') for x in xs],[F(1,4),0,1,F(3,4)])
        self.assertEqual(e.spread_rounded(-e.power2(-200),'repeat','float32'),1)
    def test_numeric_lookup(self):
        self.assertEqual(e.numeric_lookup(F(1,4),[[0,2],[2,4]]),[F(1,2),F(5,2)])
        self.assertEqual(e.numeric_lookup(1,[[0],[9]]),[9])
    def test_alpha_weighted_linear_rgb(self):
        self.assertEqual(e.linear_rgba_mix((1,0,0,0),(0,0,1,1),F(1,2)),[0,0,1,F(1,2)])
        self.assertEqual(e.linear_rgba_mix((1,0,0,0),(0,1,0,0),F(1,2)),[0,0,0,0])
    def test_bilinear_barycentric(self):
        colors=[[[0],[2]],[[4],[6]]]
        self.assertEqual(e.bilinear(colors,F(1,2),F(1,2)),[3])
        self.assertEqual(e.barycentric((F(1,4),F(1,4)),[(0,0),(1,0),(0,1)]),(F(1,2),F(1,4),F(1,4)))
    def test_bicubic_constant(self):
        colors=[[[3,-1,2] for _ in range(4)] for _ in range(4)]
        self.assertEqual(e.bicubic_patch(colors,F(3,7),F(5,9)),[3,-1,2])
    def test_grid_points(self):
        self.assertEqual(e.grid_points([0,0,2,2],2,2),[(.5,.5),(1.5,.5),(.5,1.5),(1.5,1.5)])
    def test_grid_points_halfopen_each_cell(self):
        # At this exponent the exact first center is an RN tie at its cell's
        # upper boundary. Whole-canvas containment would accept the wrong cell.
        lo=1.+2**-52; hi=1.+3*2**-52
        pts=e.grid_points([lo,0,hi,1],2,1)
        mid=(F(lo)+F(hi))/2
        self.assertLess(F(pts[0][0]),mid)
        self.assertGreaterEqual(F(pts[1][0]),mid)
        with self.assertRaises(ValueError):
            e.grid_points([1.,0,1.+2**-52,1],3,1)
    def test_correlation_direction_and_boundary(self):
        source=[[1,2,3]];kernel=[[1,0,0]]
        self.assertEqual(e.correlate_kernel(source,kernel,'zero'),[[0,1,2]])
        self.assertEqual(e.correlate_kernel(source,kernel,'reflect101'),[[2,1,2]])
        self.assertEqual(e.correlate_kernel([[7]],[[1,1,1]],'reflect101','sum'),[[7]])
    def test_width_profile(self):
        self.assertEqual(e.width_linear(25,100,[0,1],[0,10]),F(5,2))
        self.assertEqual(e.width_linear(0,0,[0,1],[3,8]),3)
        with self.assertRaises(ValueError):e.width_linear(0,1,[0,1],[-1,1])
    def test_pchip_shape_preservation(self):
        for i in range(41):
            y=e.pchip_fixture([0,1,2],[0,1,0],F(i,20))
            self.assertLessEqual(y,1);self.assertGreaterEqual(y,0)
        self.assertEqual(e.pchip_fixture([0,1],[0,10],F(1,4)),F(5,2))
    def test_grain_and_erosion(self):
        self.assertEqual(e.linear_grain([1,2,3],F(1,2),2,-1),([-1,0,1],F(1,2)))
        self.assertEqual(e.rectangle_erosion([0,0,10,6],1),(1,1,9,5))
        self.assertIsNone(e.rectangle_erosion([0,0,10,6],3))
    def test_invalid_finite_domains(self):
        with self.assertRaises(ValueError):e.frac(math.nan)
        with self.assertRaises(ValueError):e.sqrt_fraction(-1)
        with self.assertRaises(ValueError):e.linear_coordinate((0,0),(0,0),(0,0))
        with self.assertRaises(ValueError):e.correlate_kernel([[1]],[[0]],normalization='sum')


if __name__=='__main__':unittest.main()
