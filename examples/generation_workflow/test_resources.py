import copy
import json
from pathlib import Path
import unittest
import resources as r

HERE=Path(__file__).resolve().parent

class ResourceOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tile=json.loads((HERE/'rank_4x4_fixture.json').read_text())
        cls.volume=json.loads((HERE/'stbn_shape_fixture.json').read_text())
    def test_permutation_and_hash(self):
        info=r.validate_rank(self.tile['rank'],self.tile['manifest']);self.assertEqual(info['count'],16)
    def test_duplicate_rejection(self):
        bad=copy.deepcopy(self.tile['rank']);bad[0][0]=bad[0][1]
        with self.assertRaises(ValueError):r.validate_rank(bad)
    def test_hash_rejection(self):
        bad=copy.deepcopy(self.tile['manifest']);bad['sha256']='0'*64
        with self.assertRaises(ValueError):r.validate_rank(self.tile['rank'],bad)
    def test_raw_fixture_not_blue_qualified(self):
        self.assertFalse(self.tile['manifest']['blue_noise_approved'])
        self.assertFalse(self.volume['manifest']['stbn_approved'])
    def test_periodic_negative_coordinates(self):
        tile=self.tile['rank']
        self.assertEqual(r.rank_lookup(tile,-1,-1),r.rank_lookup(tile,3,3))
        self.assertEqual(r.rank_lookup(tile,1,2,(3,-1)),r.rank_lookup(tile,4,1))
    def test_midpoint_mapping(self):
        tile=self.tile['rank']
        values=[r.rank_lookup(tile,x,y) for y in range(4) for x in range(4)]
        self.assertEqual(sorted(values),[(i+.5)/16 for i in range(16)])
    def test_threshold_counts_and_nesting(self):
        previous=set()
        for k in range(17):
            pts=set(r.threshold_points(self.tile['rank'],4,4,k))
            self.assertEqual(len(pts),k);self.assertTrue(previous<=pts);previous=pts
    def test_threshold_cropped_origin(self):
        pts=r.threshold_points(self.tile['rank'],2,2,16,origin=(-1,3))
        self.assertEqual(pts,[(-.5,3.5),(.5,3.5),(-.5,4.5),(.5,4.5)])
    def test_stbn_period_not_temporal_quality(self):
        v=self.volume['rank']
        self.assertEqual(r.stbn_lookup(v,0,0,-1),r.stbn_lookup(v,0,0,1))
        self.assertEqual(r.stbn_lookup(v,0,0,0),.5/8)
        self.assertEqual(r.validate_rank(v)['count'],8)
    def test_threshold_capacity(self):
        with self.assertRaises(OverflowError):r.threshold_points(self.tile['rank'],4,4,16,max_count=3)

if __name__=='__main__':unittest.main()
