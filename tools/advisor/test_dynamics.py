import copy
import struct
import unittest
from pathlib import Path
from unittest.mock import patch
import advisor
import model_asset
import ragdoll_asset
from test_animation import fixture
ROOT=Path(__file__).resolve().parents[2]
BASE={'model':'glb','model_path':'assets/models/creator/two-bone.glb','animation':'skeletal','physics':[]}
class DynamicsTests(unittest.TestCase):
    def test_independent_feature_matrix(self):
        for rag in ('none','passive'):
            for ccd in ('none','conservative'):
                result=advisor.advise(BASE|dict(ragdoll=rag,deformation_ccd=ccd,model_physics='deforming_mesh' if ccd!='none' else 'none'))['native_build']
                self.assertEqual(result['status'],'ready');self.assertEqual(result['features']['ragdoll'],rag=='passive');self.assertEqual(result['features']['deformation_ccd'],ccd=='conservative')
    def test_invalid_combinations_rejected(self):
        for request in ({'ragdoll':'passive'},BASE|{'animation':'none','ragdoll':'passive'},BASE|{'deformation_ccd':'conservative'},BASE|{'model_physics':'static_mesh','deformation_ccd':'conservative'}):
            self.assertEqual(advisor.advise(request)['native_build']['status'],'blocked')
    def test_ragdoll_headless_adds_physics_without_mesh_collider(self):
        config=BASE|dict(ragdoll='passive',rendering='none',material='unlit',lights='none',shadows='none',reflections='none',indirect_light='none')
        result=advisor.advise(config);self.assertEqual(result['native_build']['status'],'ready');self.assertEqual(set(result['request']['physics']),{'collision','rigid_body'})
        self.assertFalse(result['native_build']['features']['model_deforming']);self.assertFalse(result['native_build']['features']['render'])
    def test_hierarchy_and_proxy_payload(self):
        cooked=model_asset.cook(advisor.normalize(BASE|{'ragdoll':'passive'})[0],ROOT);data=cooked['rig_data']
        self.assertEqual(len(data),92);self.assertEqual(data[:8],b'GERIG1\0\0');self.assertEqual(struct.unpack_from('<I',data,8)[0],2)
        self.assertEqual(struct.unpack_from('<i',data,12)[0],-1);self.assertEqual(struct.unpack_from('<i',data,52)[0],0)
        self.assertAlmostEqual(struct.unpack_from('<f',data,84)[0],1);self.assertIn('ragdoll',cooked['info'])
    def test_binary_fbx_rig_matches_glb_hierarchy(self):
        result=model_asset.cook(advisor.normalize(BASE|{'model':'fbx','model_path':'assets/models/creator/two-bone-binary.fbx','ragdoll':'passive'})[0],ROOT)
        rig=result['rig_data'];self.assertEqual(struct.unpack_from('<i',rig,12)[0],-1);self.assertEqual(struct.unpack_from('<i',rig,52)[0],0)
        self.assertAlmostEqual(struct.unpack_from('<f',rig,84)[0],1)
    def test_no_rig_payload_when_disabled(self):
        cooked=model_asset.cook(advisor.normalize(BASE)[0],ROOT);self.assertIsNone(cooked['rig_data']);self.assertNotIn('ragdoll',cooked['info'])
    def test_rig_cycles_duplicates_count_and_anchor_rejected(self):
        for mutate in (lambda a:a['rig'][0].update(parent=1),lambda a:a['rig'][1].update(node=a['rig'][0]['node']),lambda a:a['rig'][1].update(parent=99),lambda a:a['rig'][0].update(anchor=[float('nan'),0,0]),lambda a:a.update(rig=[])):
            asset=fixture();mutate(asset)
            with self.assertRaises(ValueError):ragdoll_asset.cook(asset,1,list(range(6)))
    def test_scale_applies_to_rig_and_proxy_bounds(self):
        asset=fixture();data=ragdoll_asset.cook(asset,2,list(range(6)))
        self.assertAlmostEqual(struct.unpack_from('<f',data,84)[0],2)
        with self.assertRaises(ValueError):ragdoll_asset.cook(asset,1000,list(range(6)))
if __name__=='__main__':unittest.main()
