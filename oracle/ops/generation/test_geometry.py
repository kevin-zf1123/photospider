import math
import random
import unittest
from fractions import Fraction as F
import geometry as g
import exact as e

RECT=[(0,0),(1,0),(1,1),(0,1)]
def rect(x0,y0,x1,y1):return [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]


class GeometryOracleTests(unittest.TestCase):
    def test_exact_orientation_and_near_collinear(self):
        self.assertEqual(g.orientation((0,0),(1,0),(0,1)),1)
        tiny=e.power2(-1000)
        self.assertEqual(g.orientation((0,0),(1,tiny),(2,2*tiny)),0)
        self.assertGreater(g.orientation((0,0),(1,tiny),(2,3*tiny)),0)
    def test_point_segment_and_ties(self):
        self.assertEqual(g.point_segment_distance2((2,3),(0,0),(4,0)),(9,F(1,2),(2,0)))
        self.assertEqual(g.point_segment_distance2((3,4),(0,0),(0,0)),(25,0,(0,0)))
        self.assertEqual(g.nearest_points((0,0),[(1,0),(-1,0)],[9,2]),(1,1,2))
    def test_bezier_two_independent_formulas(self):
        rng=random.Random(20260925)
        for degree in (1,2,3):
            for _ in range(60):
                cp=[(F(rng.randrange(-20,21),8),F(rng.randrange(-20,21),8)) for _ in range(degree+1)]
                t=F(rng.randrange(33),32)
                self.assertEqual(g.bezier_bernstein(cp,t),g.bezier_casteljau(cp,t))
                self.assertEqual(g.bezier_bernstein(cp,t),g.bezier_bernstein(list(reversed(cp)),1-t))
    def test_bezier_line_derivative_tangent(self):
        self.assertEqual(g.bezier_bernstein([(0,0),(3,4)],F(1,2)),(F(3,2),2))
        self.assertEqual(g.bezier_derivative([(0,0),(3,4)],F(1,2)),(3,4))
        tangent,valid=g.unit_tangent_fixture((3,4))
        self.assertEqual(tangent,(.6,.8));self.assertEqual(valid,1)
        self.assertEqual(g.unit_tangent_fixture((0,0)),((0.,0.),0))
    def test_bezier_endpoint_negative_zero(self):
        cp=[(-0.,1.),(2.,2.),(3.,3.)]
        self.assertEqual(e.float_bits(g.rounded_bezier_position(cp,0)[0]),1<<63)
        allminus=[(-0.,0.),(-0.,0.),(-0.,0.)]
        self.assertEqual(e.float_bits(g.rounded_bezier_position(allminus,F(1,2))[0]),1<<63)
    def test_split_and_trim_parameter_identity(self):
        cp=[(0,0),(1,3),(2,-1),(4,0)]
        left,right=g.split_bezier(cp,F(1,3))
        for t in (F(0),F(1,5),F(1,2),F(1)):
            self.assertEqual(g.bezier_casteljau(left,t),g.bezier_casteljau(cp,t/3))
            self.assertEqual(g.bezier_casteljau(right,t),g.bezier_casteljau(cp,F(1,3)+2*t/3))
        trimmed=g.trim_bezier_t(cp,F(1,4),F(3,4))
        self.assertEqual(g.bezier_casteljau(trimmed,F(1,2)),g.bezier_casteljau(cp,F(1,2)))
    def test_hermite_bezier_equivalence(self):
        data=[(0,1),(4,2),(3,6),(-3,0)]
        cp=[data[0],(1,3),(5,2),data[1]]
        for t in (0,F(1,7),F(1,2),1):self.assertEqual(g.hermite(data,t),g.bezier_casteljau(cp,t))
        self.assertEqual(g.hermite(data,0,True),tuple(data[2]));self.assertEqual(g.hermite(data,1,True),tuple(data[3]))
    def test_bspline_degree_one(self):
        cp=[(0,0),(3,4)]
        self.assertEqual(g.bspline_fraction(cp,[0,0,1,1],1,F(1,2)),(F(3,2),2))
        self.assertEqual(g.bspline_fraction(cp,[0,0,1,1],1,1),(3,4))
        self.assertEqual(g.bspline_fraction(cp,[0,0,1,1],1,F(1,2),derivative=True),(3,4))
    def test_bspline_nonclamped_endpoint_and_scale(self):
        cp=[(0,0),(2,2),(4,0)]
        end=g.bspline_fraction(cp,[0,1,2,3,4,5],2,1)
        self.assertEqual(end,(3,1));self.assertNotEqual(end,cp[-1])
        self.assertEqual(g.bspline_fraction(cp,[0,0,0,1,1,1],2,F(2,7),[1,2,3]),g.bspline_fraction(cp,[0,0,0,1,1,1],2,F(2,7),[7,14,21]))
    def test_bspline_degree_zero(self):
        self.assertEqual(g.bspline_fraction([(3,4)],[0,1],0,1),(3,4))
        self.assertEqual(g.bspline_fraction([(3,4)],[0,1],0,F(1,2),derivative=True),(0,0))
    def test_flatten_line_certificate(self):
        points,leaves=g.flatten_bezier([(0,0),(1,0),(2,0),(3,0)])
        self.assertEqual(points,[(0,0),(3,0)]);self.assertTrue(g.verify_flatten_certificate(leaves))
    def test_flatten_collinear_reversal_not_flat(self):
        points,leaves=g.flatten_bezier([(0,0),(2,0),(-2,0),(0,0)],F(1,20))
        self.assertGreater(len(points),2);self.assertTrue(g.verify_flatten_certificate(leaves))
    def test_flatten_continuous_control_bound_samples(self):
        cp=[(0,0),(1,3),(3,-2),(4,0)];eps=F(1,20)
        points,leaves=g.flatten_bezier(cp,eps)
        self.assertTrue(g.verify_flatten_certificate(leaves))
        for leaf in leaves:
            for t in (F(0),F(1,4),F(1,2),F(3,4),F(1)):
                curve=g.bezier_casteljau(leaf['controls'],t)
                line=g.add(g.mul(leaf['a'],1-t),g.mul(leaf['b'],t))
                self.assertLessEqual(g.norm2(g.sub(curve,line)),eps*eps)
    def test_flatten_budget_failure(self):
        with self.assertRaises(ArithmeticError):g.flatten_bezier([(0,0),(1,3),(2,0)],F(1,1000000),max_depth=0)
    def test_length_bounds_line_and_refinement(self):
        self.assertEqual(g.bezier_length_bounds([(0,0),(3,4)],0),(5,5))
        cp=[(0,0),(1,3),(3,0)]
        l0,u0=g.bezier_length_bounds(cp,0);l3,u3=g.bezier_length_bounds(cp,3)
        self.assertGreaterEqual(l3,l0);self.assertLessEqual(u3,u0)
    def test_uniform_t(self):
        out=g.uniform_t_samples([(0,0),(0,0),(4,0)],3)
        self.assertEqual([p for t,p in out],[(0,0),(1,0),(4,0)])
    def test_uniform_arc_open_closed_zero(self):
        out=g.polyline_arc_samples([(0,0),(100,0)],5)
        self.assertEqual([row[1] for row in out],[(0,0),(25,0),(50,0),(75,0),(100,0)])
        square=rect(0,0,100,100)
        self.assertEqual([r[1] for r in g.polyline_arc_samples(square,4,True)],square)
        self.assertEqual([r[1] for r in g.polyline_arc_samples([(2,3)],3)],[(2,3)]*3)
    def test_arc_join_after_segment_selection(self):
        out=g.polyline_arc_samples([(0,0),(5,0),(10,0)],3)
        self.assertEqual(out[1][2:],(1,F(0)))
    def test_spacing_exact_endpoint(self):
        out=g.polyline_spacing_samples([(0,0),(100,0)],30)
        self.assertEqual([r[0] for r in out],[0,30,60,90,100])
        self.assertEqual(len(g.polyline_spacing_samples([(0,0),(100,0)],25)),5)
        with self.assertRaises(NotImplementedError):g.polyline_arc_samples([(0,0),(1,1)],3)
    def test_trim_dash(self):
        self.assertEqual(g.trim_polyline([(0,0),(100,0)],25,75),[(25,0),(75,0)])
        self.assertEqual(g.dash_intervals(10,[3,2]),[(0,3),(5,8)])
        self.assertEqual(g.dash_intervals(10,[3,2],1),[(0,2),(4,7),(9,10)])
        self.assertEqual(g.dash_intervals(10,[3]),[(0,3),(6,9)])
        self.assertEqual(g.dash_intervals(10,[0,2]),[])
        self.assertEqual(g.dash_intervals(10,[3,0]),[(0,10)])
        with self.assertRaises(ValueError):g.dash_intervals(10,[0,0])
    def test_simplify_and_smooth(self):
        self.assertEqual(g.simplify_polyline([(0,0),(1,0),(2,0)],0),[(0,0),(2,0)])
        self.assertEqual(g.simplify_polyline([(0,0),(2,0),(0,0)],0),[(0,0),(2,0),(0,0)])
        self.assertEqual(g.simplify_polyline([(0,0),(1,0),(2,0)],0,locked=[1]),[(0,0),(1,0),(2,0)])
        self.assertEqual(g.chaikin([(0,0),(4,0)]),[(0,0),(1,0),(3,0),(4,0)])
        self.assertEqual(len(g.chaikin(RECT,1,True)),8)
    def test_constructor_and_empty(self):
        self.assertTrue(g.validate_core_fixture(['M','L'],[(0,0),(3,4)],[0,1,2],[0,2],[False]))
        self.assertTrue(g.validate_core_fixture([],[],[0],[0],[]))
        self.assertTrue(g.validate_core_fixture(['M'],[(0,0)],[0,1],[0,1],[False]))
        with self.assertRaises(ValueError):g.validate_core_fixture(['M','L'],[(0,0),(3,4)],[0,1,2],[0,2],[True])
    def test_concat_split_reverse(self):
        self.assertEqual(g.concat_paths([['a','b'],[],['c']]),['a','b','c'])
        self.assertEqual(g.split_subpaths(['a','b','c'],[1,0,1],2),(['b','a','c'],[0,1,3]))
        self.assertEqual(g.reverse_polyline(g.reverse_polyline(RECT,True),True),RECT)
    def test_arc_validation_and_fit_necessary_only(self):
        self.assertTrue(g.validate_arc_fixture((0,0),(1,0),(0,1),0,direction=1))
        with self.assertRaises(ValueError):g.validate_arc_fixture((0,0),(1,0),(2,0),0,direction=1)
        self.assertTrue(g.cubic_candidate_checks([(0,0),(100,100),(-100,100),(1,0)],[(0,0),(1,0)]))
        # Passing this necessary endpoint test intentionally does NOT imply
        # a Hausdorff bound: the control polygon is wildly displaced.
    def test_affine_identity_nonuniform(self):
        p=[(-0.,1.),(3.,4.)]
        self.assertEqual(e.float_bits(g.affine_transform(p,[[1,0,0],[0,1,0]])[0][0]),1<<63)
        outline=g.straight_stroke_rectangles((0,0),(10,0),2)[0]
        scaled=g.affine_transform(outline,[[1,0,0],[0,3,0]])
        self.assertEqual(g.polygon_area(scaled),60)
        centerline=g.affine_transform([(0,0),(10,0)],[[1,0,0],[0,3,0]])
        self.assertEqual(g.polygon_area(g.straight_stroke_rectangles(*centerline,2)[0]),20)
    def test_exact_fill_basic(self):
        self.assertEqual(g.polygon_pixel_area([RECT]),1)
        self.assertEqual(g.polygon_pixel_area([rect(F(1,4),F(1,4),F(3,4),F(3,4))]),F(1,4))
        self.assertEqual(g.polygon_pixel_area([[(0,0),(1,1),(0,1),(1,0)]]),F(1,2))
    def test_fill_rule_duplicate_and_hole(self):
        self.assertEqual(g.polygon_pixel_area([RECT,RECT],'nonzero') if False else g.polygon_pixel_area([RECT,RECT]),1)
        self.assertEqual(g.polygon_pixel_area([RECT,RECT],fill_rule='evenodd'),0)
        hole=list(reversed(rect(F(1,4),F(1,4),F(3,4),F(3,4))))
        self.assertEqual(g.polygon_pixel_area([RECT,hole]),F(3,4))
    def test_fill_shared_edges_and_zero_area(self):
        a=rect(0,0,F(1,2),1);b=rect(F(1,2),0,1,1)
        self.assertEqual(g.polygon_pixel_area([a,b]),1)
        self.assertEqual(g.polygon_pixel_area([[(0,0),(1,0),(0,0)]]),0)
        self.assertEqual(g.polygon_pixel_area([[(0,0)]]),0)
    def test_exact_fill_slab_not_supersample(self):
        tiny=e.power2(-100)
        self.assertEqual(g.polygon_pixel_area([rect(0,0,tiny,1)]),tiny)
    def test_fill_sum_area_and_translation(self):
        polygon=[(F(1,3),F(1,4)),(F(8,3),F(1,4)),(F(7,3),F(9,4)),(F(2,3),F(11,4))]
        area=sum(g.polygon_pixel_area([polygon],(x,y)) for y in range(3) for x in range(3))
        self.assertEqual(area,g.polygon_area(polygon))
        translated=[(x-100,y+200) for x,y in polygon]
        self.assertEqual(g.polygon_pixel_area([polygon],(1,1)),g.polygon_pixel_area([translated],(-99,201)))
    def test_fill_random_rectangles_against_analytic(self):
        rng=random.Random(123)
        for _ in range(50):
            xs=sorted(rng.sample(range(-8,17),2));ys=sorted(rng.sample(range(-8,17),2))
            b=[F(xs[0],8),F(ys[0],8),F(xs[1],8),F(ys[1],8)]
            self.assertEqual(g.polygon_pixel_area([rect(*b)]),e.rectangle_coverage(b))
    def test_region_distance_internal_edge_regression(self):
        source=[rect(0,0,1,2),rect(1,0,2,2)]
        self.assertEqual(g.region_sdf((1,1),source),-1)
        self.assertEqual(g.region_sdf((2,1),source),0)
        self.assertEqual(g.region_sdf((3,1),source),1)
    def test_region_distance_hole_and_empty(self):
        source=[rect(0,0,4,4),list(reversed(rect(1,1,3,3)))]
        self.assertEqual(g.region_sdf((2,2),source),1)
        self.assertEqual(g.region_sdf((F(1,2),2),source),-.5)
        self.assertEqual(g.region_sdf((0,0),[RECT,RECT],'evenodd',5),5)
        with self.assertRaises(ValueError):g.region_sdf((0,0),[RECT,RECT],'evenodd')
    def test_boolean_rectangle_areas(self):
        expected={'union':6,'intersection':2,'difference':2,'xor':4}
        for op,area in expected.items():
            out=g.boolean_axis_aligned((0,0,2,2),(1,0,3,2),op)
            self.assertEqual(sum(g.polygon_area(c) for c in out),area)
    def test_boolean_point_and_edge_contacts(self):
        edge=g.boolean_axis_aligned((0,0,1,1),(1,0,2,1),'union')
        self.assertEqual(edge,[[(0,0),(2,0),(2,1),(0,1)]])
        point=g.boolean_axis_aligned((0,0,1,1),(1,1,2,2),'intersection')
        self.assertEqual(point,[])
        union=g.boolean_axis_aligned((0,0,1,1),(1,1,2,2),'union')
        self.assertEqual(len(union),2)
    def test_boolean_hole_and_idempotence(self):
        out=g.boolean_axis_aligned((0,0,4,4),(1,1,3,3),'difference')
        self.assertEqual(sorted(g.polygon_area(c) for c in out),[-4,16])
        self.assertEqual(g.boolean_regions([RECT],[RECT],'xor'),[])
        self.assertEqual(g.boolean_regions([RECT],[RECT],'union'),[[tuple(map(F,p)) for p in RECT]])
    def test_boolean_rational_intersection(self):
        a=[[(0,0),(3,0),(0,3)]];b=[[(0,1),(3,1),(3,4)]]
        out=g.boolean_regions(a,b,'intersection')
        self.assertEqual(sum(g.polygon_area(c) for c in out),1)
        self.assertEqual(sum(g.polygon_pixel_area(out,(x,y)) for y in range(4) for x in range(4)),1)
    def test_publication_merge_rejection(self):
        base=F(1);tiny=e.power2(-60)
        with self.assertRaises(ArithmeticError):g.publish_polygon_fixture([rect(base,0,base+tiny,1)],F(1,1000))
        self.assertEqual(g.publish_polygon_fixture([RECT],F(1,1000)),[[tuple(map(F,p)) for p in RECT]])
    def test_stroke_and_variable_sweep(self):
        self.assertEqual(g.polygon_area(g.straight_stroke_rectangles((0,0),(10,0),2)[0]),20)
        self.assertEqual(g.straight_stroke_rectangles((0,0),(0,0),2,'butt'),[])
        self.assertEqual(g.polygon_area(g.straight_stroke_rectangles((0,0),(0,0),2,'square')[0]),4)
        self.assertTrue(g.variable_sweep_contains((1,1),(0,0),(2,0),1,1))
        self.assertFalse(g.variable_sweep_contains((1,2),(0,0),(2,0),1,1))
        # Large end disc contains every intermediate disc.
        self.assertTrue(g.variable_sweep_contains((1,2),(0,0),(1,0),0,2))
    def test_overlapping_stroke_union_not_layer_sum(self):
        contours=g.straight_stroke_rectangles((-1,F(1,2)),(2,F(1,2)),1)*2
        self.assertEqual(g.polygon_pixel_area(contours),1)


if __name__=='__main__':unittest.main()
