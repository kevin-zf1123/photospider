import unittest
from fractions import Fraction as F
try:
    from oracle.ops.generation.gen05.measured import angular as m_angular
    from oracle.ops.generation.pth02.measured import arc_evaluate as m_arc_evaluate
    from oracle.ops.generation.pth03.measured import arc_length as m_arc_length
    from oracle.ops.generation.pth03.measured import bezier_length as m_bezier_length
    from oracle.ops.generation.pth03.measured import bspline_length as m_bspline_length
    from oracle.ops.generation.pth11.measured import closest_bezier as m_closest_bezier
    from oracle.ops.generation.gen03.measured import ellipse_coverage as m_ellipse_coverage
    from oracle.ops.generation.gen03.measured import ellipse_distance as m_ellipse_distance
    from oracle.ops.generation.noi02.measured import gaussian as m_gaussian
    from oracle.ops.generation.noi03.measured import l2_kernel as m_l2_kernel
    from oracle.ops.generation.noi08.measured import poisson_cdf as m_poisson_cdf
    from oracle.ops.generation.noi08.measured import poisson_quantile as m_poisson_quantile
    from oracle.ops.generation.common.measured import precision_agreement as m_precision_agreement
    from oracle.ops.generation.gen04.measured import siemens_star as m_siemens_star
    from oracle.ops.generation.noi09.measured import speckle as m_speckle
    from oracle.ops.generation.gen03.measured import star_vertices as m_star_vertices
    from oracle.ops.generation.gen05.measured import two_circle as m_two_circle
    from oracle.ops.generation.gen04.measured import zone_plate as m_zone_plate
    import mpmath as mp
    AVAILABLE=True
except ImportError:
    AVAILABLE=False
from oracle.ops.generation.pth03.geometry import bezier_length_bounds as g_bezier_length_bounds
from oracle.ops.generation.noi02.rng import gaussian_inputs as rng_gaussian_inputs


@unittest.skipUnless(AVAILABLE,'optional mpmath dependency not installed')
class MeasuredDiagnosticTests(unittest.TestCase):
    def test_gaussian_degenerate_and_phase(self):
        self.assertEqual(m_gaussian(3,0,F(1,2),0),3)
        z=m_gaussian(0,1,F(1,2),F(1,4))
        self.assertLess(abs(z),mp.mpf('1e-75'))
    def test_gaussian_two_precision_check(self):
        u,v=rng_gaussian_inputs(2,-3,seed=7)
        report=m_precision_agreement(m_gaussian,1,2,u,v)
        self.assertTrue(report['same_binary64']);self.assertEqual(report['quality'],'Measured_not_certified')
    def test_zone_plate_and_siemens(self):
        self.assertEqual(m_zone_plate((3,4),(0,0),0),1)
        self.assertEqual(m_siemens_star((0,0),(0,0),4),F(1,2))
        self.assertAlmostEqual(float(m_siemens_star((1,0),(0,0),4)),1)
    def test_angular_cardinal(self):
        for p,q in [((1,0),0),((0,1),.25),((-1,0),.5),((0,-1),.75)]:
            self.assertAlmostEqual(float(m_angular(p,(0,0))),q,places=15)
        self.assertEqual(m_angular((0,0),(0,0)),0)
    def test_star_vertex_structure(self):
        pts=m_star_vertices((0,0),2,1,4)
        self.assertEqual(len(pts),8);self.assertEqual(pts[0],(2.,0.))
    def test_ellipse_area_known(self):
        got=m_ellipse_coverage((F(1,2),F(1,2)),(F(1,2),F(1,2)))
        with mp.workdps(80):self.assertLess(abs(got-mp.pi/4),mp.mpf('1e-70'))
        self.assertEqual(m_ellipse_coverage((10,10),(1,2)),0)
    def test_ellipse_distance_non_radial_case(self):
        self.assertEqual(m_ellipse_distance((0,0),(0,0),(2,1)),-1)
        self.assertAlmostEqual(float(m_ellipse_distance((3,0),(0,0),(2,1))),1,places=14)
        # Interior x-axis closest point is not the radial intersection at x=2.
        self.assertLess(abs(float(m_ellipse_distance((F(1,2),0),(0,0),(2,1)))),1.5)
    def test_two_circle_analytic(self):
        self.assertEqual(m_two_circle((1,0),(0,0),0,(0,0),2),F(1,2))
    def test_arc_fullturn_identity_and_quarter(self):
        p0,d0=m_arc_evaluate((0,0),(1,0),(0,1),0,0,direction=1)
        p1,d1=m_arc_evaluate((0,0),(1,0),(0,1),0,1,direction=1)
        self.assertEqual(p0,p1);self.assertEqual(d0,d1)
        pq,_=m_arc_evaluate((0,0),(1,0),(0,1),0,F(1,4),direction=1)
        self.assertLess(abs(pq[0]),mp.mpf('1e-75'));self.assertEqual(pq[1],1)
    def test_arc_length_circle(self):
        L=m_arc_length((1,0),(0,1),direction=1)
        with mp.workdps(80):self.assertLess(abs(L-2*mp.pi),mp.mpf('1e-70'))
    def test_bezier_length_measured_inside_exact_bounds(self):
        cp=[(0,0),(1,2),(3,0)];lo,hi=g_bezier_length_bounds(cp,5)
        value=m_bezier_length(cp)
        with mp.workdps(80):
            self.assertLessEqual(mp.mpf(lo.numerator)/lo.denominator,value)
            self.assertGreaterEqual(mp.mpf(hi.numerator)/hi.denominator,value)
        self.assertEqual(m_bezier_length([(0,0),(3,4)]),5)
    def test_bspline_length_line(self):
        self.assertEqual(m_bspline_length([(0,0),(3,4)],[0,0,1,1],1),5)
    def test_closest_bezier_line(self):
        d,t,p=m_closest_bezier((2,3),[(0,0),(4,0)])
        self.assertEqual(d,3);self.assertEqual(t,F(1,2));self.assertEqual(p,(2,0))
    def test_poisson_quantile_cdf_and_degenerate(self):
        self.assertEqual(m_poisson_quantile(0,F(1,2)),0)
        self.assertEqual(m_poisson_quantile(1,F(1,2)),1)
        for lam in (F(1,10),1,5,20):
            for u in (F(1,10),F(1,2),F(9,10)):
                k=m_poisson_quantile(lam,u)
                self.assertEqual(k,m_poisson_quantile(lam,u,dps=160))
                self.assertLess(m_poisson_cdf(lam,k-1),float(u))
                self.assertGreaterEqual(m_poisson_cdf(lam,k),float(u))
        with self.assertRaises(NotImplementedError):m_poisson_quantile(1001,F(1,2))
    def test_speckle_zero_and_integer_looks(self):
        self.assertEqual(m_speckle(0,[F(1,2)]),0)
        z=m_speckle(1,[F(1,2),F(1,2)])
        with mp.workdps(80):self.assertLess(abs(z-mp.log(2)),mp.mpf('1e-70'))
    def test_l2_kernel_exact_special(self):
        self.assertEqual(m_l2_kernel([[1,2,3]],[[0,1,0]]),[[1,2,3]])
    def test_measured_cardinal_is_not_strict_certificate(self):
        # High precision still leaves a nonzero cos(pi/2) residual. Strict must
        # use a proved analytic/interval path, not merely round this residual.
        a=m_gaussian(0,1,F(1,2),F(1,4),dps=80)
        b=m_gaussian(0,1,F(1,2),F(1,4),dps=160)
        self.assertNotEqual(float(a),float(b))


if __name__=='__main__':unittest.main()
