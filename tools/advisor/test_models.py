import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import advisor
import build_runtime
import model_asset
from runtime_plan import emit

OBJ='v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
TETRA='v -1 0 -1\nv 1 0 -1\nv 0 0 1\nv 0 2 0\nf 1 2 4\nf 2 3 4\nf 3 1 4\nf 1 3 2\n'
BASE={'model':'obj','model_path':'assets/models/creator/tetrahedron.obj','physics':[]}
class ModelTests(unittest.TestCase):
    def test_model_features_are_independent(self):
        for physics in ('none','convex_hull','static_mesh'):
            for ray in ('none','triangles'):
                request=BASE|{'model_physics':physics,'model_ray_tracing':ray}
                if ray=='triangles':request.update(rendering='hybrid',shadows='ray_traced')
                p=advisor.advise(request)['native_build'];self.assertEqual(p['status'],'ready')
                self.assertEqual(p['features']['rigid_body'],physics!='none')
                self.assertEqual(p['features']['model_convex'],physics=='convex_hull')
                self.assertEqual(p['features']['model_static'],physics=='static_mesh')
                self.assertEqual(p['features']['rt_mesh'],ray=='triangles')
    def test_disabled_model_rejects_ignored_settings(self):
        for x in [{'model_path':'a.obj'},{'model_scale':2},{'model_physics':'convex_hull'},{'model_ray_tracing':'triangles'}]:
            with self.subTest(x=x),self.assertRaises(ValueError):advisor.advise(x)
    def test_mesh_ray_requires_hybrid_effect(self):
        with self.assertRaises(ValueError):advisor.advise(BASE|{'model_ray_tracing':'triangles'})
    def test_headless_model_requires_physics(self):
        req=BASE|{'rendering':'none','material':'unlit','lights':'none','indirect_light':'none'}
        self.assertEqual(advisor.advise(req)['native_build']['status'],'blocked')
        self.assertEqual(advisor.advise(req|{'model_physics':'static_mesh'})['native_build']['status'],'ready')
    def test_paths_cannot_escape_or_inject_build(self):
        for value in ['../x.obj','C:/x.obj','/x.obj','a/../../x.obj','a\\x.obj','',5,'x.glb']:
            with self.subTest(value=value),self.assertRaises(ValueError):advisor.advise(BASE|{'model_path':value})
    def test_scale_is_bounded_finite_number(self):
        for value in [True,'1',0,-1,float('nan'),float('inf'),1001]:
            with self.subTest(value=value),self.assertRaises(ValueError):advisor.advise(BASE|{'model_scale':value})
    def test_negative_references_and_unused_vertices(self):
        text='v 99 99 99\n'+OBJ.replace('f 1 2 3','f -3/-1/-1 -2//-1 -1')
        vertices,indices=model_asset.parse_obj(text,2)
        self.assertEqual(vertices,[(0,0,0),(2,0,0),(0,2,0)]);self.assertEqual(indices,[0,1,2])
    def test_bad_geometry_never_silently_becomes_box(self):
        cases=[OBJ.replace('f 1 2 3','f 1 1 3'),OBJ.replace('f 1 2 3','f 0 2 3'),OBJ.replace('f 1 2 3','f 1 2 4'),
               OBJ.replace('f 1 2 3','f 1 2 3 1'),OBJ.replace('v 1 0 0','v nan 0 0'),OBJ.replace('v 1 0 0','v 1e40 0 0'),
               OBJ.replace('f 1 2 3','f 1x 2 3'),'v 0 0 0\n','curv 1 2 3']
        for text in cases:
            with self.subTest(text=text),self.assertRaises(ValueError):model_asset.parse_obj(text)
    def test_material_paths_are_not_loaded(self):
        v,i=model_asset.parse_obj('mtllib ../../private.mtl\nusemtl ignored\n'+OBJ)
        self.assertEqual(len(i),3)
    def test_convex_requires_volume(self):
        v,_=model_asset.parse_obj(OBJ)
        with self.assertRaises(ValueError):model_asset.require_volume(v)
        v,_=model_asset.parse_obj(TETRA);model_asset.require_volume(v)
    def test_asset_content_changes_fingerprint(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);file=root/'model.obj';file.write_text(OBJ)
            cfg=advisor.normalize(BASE|{'model_path':'model.obj'})[0];a=model_asset.cook(cfg,root)
            self.assertEqual(a['data'][:8],b'GEMESH1\0');self.assertEqual(struct.unpack('<II',a['data'][8:16]),(3,1))
            file.write_text(OBJ.replace('v 1 0 0','v 2 0 0'));b=model_asset.cook(cfg,root)
            self.assertNotEqual(a['info']['mesh_sha256'],b['info']['mesh_sha256'])
    def test_missing_model_fails_before_cmake(self):
        with patch('build_runtime.find_cmake') as cmake:
            with self.assertRaises(OSError):build_runtime.build(BASE|{'model_path':'assets/models/not-present.obj'})
            cmake.assert_not_called()
    def test_emit_packages_geometry_and_tracks_source(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);path=folder/'config.json';path.write_text(json.dumps(BASE))
            emit(path,folder/'generated');header=(folder/'generated/SelectedFeatures.h').read_text()
            self.assertIn('#define ENGINE_MODEL_GEOMETRY 1',header)
            self.assertIn('#define ENGINE_MODEL_CONVEX 0',header)
            self.assertIn('tetrahedron.obj',(folder/'generated/features.cmake').read_text())
            self.assertTrue((folder/'generated/model.mesh').is_file())

if __name__=='__main__':unittest.main()
