"""Exact deterministic acceptance inputs for the independent slab oracle."""
from fractions import Fraction as F

def p(points):
    return [(F(x), F(y)) for x, y in points]

def r(a, b, c, d):
    return p([(a,b),(c,b),(c,d),(a,d)])

def case(name, a, b, operation, area, rule="nonzero"):
    return dict(name=name,a=a,b=b,operation=operation,expected_area=F(area),
                fill_rule_a=rule,fill_rule_b="nonzero")

FIXTURES = [
    case("rational_triangle", [p([(0,0),(6,0),(0,4)])], [r(1,-1,5,3)], "intersection", F(95,12)),
    case("bowtie", [p([(0,0),(4,4),(0,4),(4,0)])], [], "union", 8, "evenodd"),
    case("evenodd_duplicate", [r(0,0,4,4),r(0,0,4,4)], [p([(1,1),(3,1),(2,3)])], "union", 2, "evenodd"),
    case("opposite_winding", [r(0,0,4,4),p([(0,0),(0,4),(4,4),(4,0)])], [r(2,-1,6,2)], "union", 12),
    case("donut_cut", [r(0,0,8,8),p([(2,2),(2,6),(6,6),(6,2)])], [r(3,-1,5,9)], "difference", 40),
    case("collinear_t", [r(0,0,2,4)], [r(2,1,5,3)], "union", 14),
    case("collinear_point_contact", [r(0,0,2,2)], [p([(1,0),(0,-1),(2,-1)])], "union", 5),
    case("point_contact", [p([(-2,0),(0,-2),(2,0),(0,2)])], [p([(2,0),(4,-2),(6,0),(4,2)])], "intersection", 0),
    case("edge_contact", [r(0,0,3,2)], [r(1,2,4,5)], "intersection", 0),
    case("nonzero_nested", [r(0,0,10,10),r(2,2,8,8)], [r(4,-1,6,11)], "difference", 80),
    case("xor_point_contacts", [r(0,0,4,4)], [r(2,2,6,6)], "xor", 24),
    case("dyadic_sliver", [r(0,0,1,1)], [r(0,0,1-F(1,1<<50),1)], "difference", F(1,1<<50)),
    case("evenodd_island", [r(0,0,12,12),r(2,2,10,10),r(4,4,8,8)], [r(5,-1,7,13)], "intersection", 16, "evenodd"),
]
