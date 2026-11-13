import copy
import unittest
from unittest.mock import patch
from pathlib import Path
import advisor
import model_asset
from test_animation import fixture
ROOT=Path(__file__).resolve().parents[2]
BASE={'model':'glb','model_path':'assets/models/creator/two-bone.glb','animation':'skeletal','model_physics':'deforming_mesh','physics':[]}
class DeformingTests(unittest.TestCase):
    def test_enabled_adds_required_modules(self):
        result=advisor.advise(BASE);self.assertEqual(result['native_build']['status'],'ready')
        self.assertEqual(set(result['request']['physics']),{'collision','rigid_body'})
        flags=result['native_build']['features']
        self.assertTrue(all(flags[k] for k in ('model_deforming','model_geometry','model_animation','rigid_body','collision')))
        self.assertFalse(flags['model_static']);self.assertFalse(flags['model_convex'])
    def test_disabled_omits_deforming_module_and_unneeded_physics(self):
        flags=advisor.advise(BASE|{'model_physics':'none'})['native_build']['features']
        self.assertFalse(flags['model_deforming']);self.assertFalse(flags['rigid_body']);self.assertTrue(flags['model_animation'])
        self.assertFalse(advisor.advise({})['native_build']['features']['model_deforming'])
    def test_requires_skeleton_and_supported_model(self):
        for change in ({'animation':'none'},{'model':'obj','model_path':'x.obj'},{'model_physics':'static_mesh'},{'model_physics':'convex_hull'}):
            self.assertEqual(advisor.advise(BASE|change)['native_build']['status'],'blocked')
    def test_physics_only_skeleton_is_supported(self):
        base=BASE|dict(rendering='none',material='unlit',lights='none',shadows='none',reflections='none',indirect_light='none')
        result=advisor.advise(base)['native_build'];self.assertEqual(result['status'],'ready');self.assertFalse(result['features']['render'])
        self.assertEqual(advisor.advise(base|{'model_physics':'none'})['native_build']['status'],'blocked')
    def test_fbx_and_rt_signed_are_independent_choices(self):
        result=advisor.advise(BASE|dict(model='fbx',model_path='a.fbx',material='pbr',rendering='hybrid',shadows='ray_traced',model_ray_tracing='triangles',distribution='signed'))['native_build']
        self.assertEqual(result['status'],'ready');self.assertTrue(result['features']['rt_mesh']);self.assertTrue(result['features']['distribution_signature'])
    def test_cooker_enforces_triangle_budget_only_when_selected(self):
        asset=fixture();asset['indices']=[0,2,1]*4097
        with patch('gltf_asset.load',return_value=asset):
            with self.assertRaisesRegex(ValueError,'4096'):model_asset.cook(advisor.normalize(BASE)[0],ROOT)
            cooked=model_asset.cook(advisor.normalize(BASE|{'model_physics':'none'})[0],ROOT)
            self.assertEqual(cooked['info']['triangles'],4097)
    def test_limit_boundary_and_both_payloads(self):
        asset=fixture();asset['indices']=[0,2,1]*4096
        with patch('gltf_asset.load',return_value=asset):
            cooked=model_asset.cook(advisor.normalize(BASE)[0],ROOT)
            self.assertEqual(cooked['info']['triangles'],4096);self.assertTrue(cooked['animation_data'])
if __name__=='__main__':unittest.main()
