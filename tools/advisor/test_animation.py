import copy
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import advisor
import gltf_asset
import model_asset
from model_fixtures import glb_document,pack_glb

ROOT=Path(__file__).resolve().parents[2]
BASE={'model':'glb','model_path':'assets/models/creator/two-bone.glb','physics':[],'animation':'skeletal','animation_clip':'Swing'}
def fixture():
    d,b=glb_document();return gltf_asset.load(pack_glb(d,b),True,'Swing')
def deformed(asset,frame,vertex):
    p=asset['base'][vertex];j=asset['joints'][vertex];w=asset['weights'][vertex]
    return [sum(gltf_asset.point(asset['frames'][frame][j[k]],p)[a]*w[k] for k in range(4)) for a in range(3)]

class AnimationTests(unittest.TestCase):
    def test_joint_hierarchy_inverse_bind_and_weight_blending(self):
        a=fixture();self.assertEqual(len(a['frames']),121)
        for p,q in zip(a['positions'],a['base']):
            for x,y in zip(p,q):self.assertAlmostEqual(x,y,places=6)
        # t=1: root translates +1, tip rotates 45 degrees around (0,1,0).
        tip=deformed(a,60,4);self.assertAlmostEqual(tip[0],1-1.4/math.sqrt(2),places=6)
        self.assertAlmostEqual(tip[1],1+.6/math.sqrt(2),places=6)
        mid=deformed(a,60,2);self.assertAlmostEqual(mid[0],1-.2-.2/math.sqrt(2),places=6)
        self.assertAlmostEqual(mid[1],1-.2/math.sqrt(2),places=6)
    def test_clip_selection_is_explicit(self):
        d,b=glb_document();a=gltf_asset.load(pack_glb(d,b),True,'Lift')
        self.assertEqual(a['clip'],'Lift');self.assertAlmostEqual(deformed(a,60,4)[1],3)
        with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b),True,'missing')
        d['animations'][1]['name']='Swing'
        with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b),True,'Swing')
    def test_rest_static_has_no_animation_payload(self):
        cfg=advisor.normalize(BASE|{'animation':'none','animation_clip':''})[0];c=model_asset.cook(cfg,ROOT)
        self.assertIsNone(c['animation_data']);self.assertNotIn('animation',c['info'])
        self.assertFalse(advisor.advise(cfg)['native_build']['features']['model_animation'])
    def test_animation_and_format_are_independent_gates(self):
        for fmt in ('glb','fbx'):
            req=BASE|{'model':fmt,'model_path':'model.'+fmt}
            self.assertEqual(advisor.advise(req)['native_build']['status'],'ready')
        for request in ({'animation':'skeletal'},BASE|{'model':'obj','model_path':'x.obj'},BASE|{'model_physics':'convex_hull'}):
            self.assertEqual(advisor.advise(request)['native_build']['status'],'blocked')
        self.assertEqual(advisor.advise(BASE|{'physics':['rigid_body']})['native_build']['status'],'ready')
    def test_reject_unused_or_mismatched_fields(self):
        for request in ({'animation_clip':'Swing'},BASE|{'model_path':'model.fbx'},BASE|{'animation_clip':False},BASE|{'animation_clip':'a\0'}):
            with self.assertRaises(ValueError):advisor.advise(request)
    def test_cook_scale_applies_to_positions_and_animated_translation(self):
        a=model_asset.cook(advisor.normalize(BASE|{'model_scale':2})[0],ROOT);data=a['animation_data']
        self.assertEqual(data[:8],b'GEANIM1\0');self.assertEqual(struct.unpack_from('<III',data,8),(6,2,121))
        self.assertAlmostEqual(struct.unpack_from('<f',data,24+6*36+60*2*64+12*4)[0],2)
        self.assertEqual(a['info']['animation']['clip'],'Swing')
    def test_animation_content_changes_digest_without_geometry_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'model.glb';d,b=glb_document();p.write_bytes(pack_glb(d,b));cfg=advisor.normalize(BASE|{'model_path':'model.glb'})[0]
            a=model_asset.cook(cfg,Path(tmp));cfg['animation_clip']='Lift';c=model_asset.cook(cfg,Path(tmp))
            self.assertEqual(a['data'],c['data']);self.assertNotEqual(a['animation_data'],c['animation_data'])
    def test_no_fbx_tool_or_legacy_importer_for_glb_or_disabled(self):
        with patch('fbx_asset.load') as tool:
            model_asset.cook(advisor.normalize(BASE)[0],ROOT);model_asset.cook(advisor.normalize({})[0],ROOT);tool.assert_not_called()
    def test_malformed_glb_never_reads_external_buffer(self):
        d,b=glb_document();d['buffers'][0]['uri']='../../secret.bin'
        with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b))
        raw=pack_glb(*glb_document())
        for invalid in (raw[:-1],raw+b'x',b'no glb',raw[:4]+struct.pack('<I',1)+raw[8:]):
            with self.assertRaises(ValueError):gltf_asset.load(invalid)
    def test_accessor_bounds_sparse_and_extensions_rejected(self):
        for mutate in (lambda d:d['accessors'][0].update(byteOffset=99999),lambda d:d['bufferViews'][0].update(byteStride=1),lambda d:d['accessors'][0].update(sparse={}),lambda d:d.update(extensionsRequired=['KHR_draco_mesh_compression']),lambda d:d['meshes'][0]['primitives'][0].update(mode=5)):
            d,b=glb_document();mutate(d)
            with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b))
    def test_hierarchy_and_skin_bounds_rejected(self):
        for mutate in (lambda d:d['nodes'][2].update(children=[1]),lambda d:d['skins'][0].update(joints=[1,99]),lambda d:d['nodes'][0].update(skin=9),lambda d:d['nodes'][1].update(matrix=gltf_asset.IDENTITY)):
            d,b=glb_document();mutate(d)
            with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b),True)
    def test_morph_and_more_than_four_influences_rejected(self):
        for key,value in [('targets',[{}]),('attributes',{'POSITION':0,'JOINTS_0':2,'WEIGHTS_0':3,'JOINTS_1':2})]:
            d,b=glb_document();d['meshes'][0]['primitives'][0][key]=value
            with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b),True)
    def test_interpolation_endpoints_step_cubic_and_shortest_rotation(self):
        sample=gltf_asset.sample
        self.assertEqual(sample([0,1],[[1,0,0],[3,0,0]],.5,'STEP',False),[1,0,0])
        self.assertEqual(sample([0,1],[[0,0,0],[1,0,0],[0,0,0],[0,0,0],[3,0,0],[0,0,0]],.5,'CUBICSPLINE',False),[2,0,0])
        self.assertEqual(sample([0,1],[[0,0,0,1],[0,0,0,-1]],.5,'LINEAR',True),[0,0,0,1])
    def test_node_transform_and_instanced_geometry(self):
        d,b=glb_document();d['nodes'][0].pop('skin');d['nodes'][0]['translation']=[3,0,0]
        attrs=d['meshes'][0]['primitives'][0]['attributes'];attrs.pop('JOINTS_0');attrs.pop('WEIGHTS_0')
        a=gltf_asset.load(pack_glb(d,b));self.assertAlmostEqual(a['positions'][0][0],2.6)
    def test_static_degenerate_faces_are_removed_and_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);d,b=glb_document();a=d['accessors'][1];view=d['bufferViews'][a['bufferView']]
            struct.pack_into('<H',b,view['byteOffset']+2,0) # first face repeats vertex 0
            (root/'model.glb').write_bytes(pack_glb(d,b))
            cfg=advisor.normalize(BASE|{'model_path':'model.glb','animation':'none','animation_clip':''})[0]
            result=model_asset.cook(cfg,root)
            self.assertEqual(result['info']['triangles'],3);self.assertEqual(result['info']['discarded_degenerate_triangles'],1)
            cfg.update(animation='skeletal',animation_clip='Swing')
            with self.assertRaises(ValueError):model_asset.cook(cfg,root)

    def test_invalid_time_and_matrices_rejected(self):
        d,b=glb_document();ac=d['accessors'][5];offset=d['bufferViews'][ac['bufferView']]['byteOffset'];struct.pack_into('<f',b,offset+4,-1)
        with self.assertRaises(ValueError):gltf_asset.load(pack_glb(d,b),True)
        a=fixture();a['frames'][0][0][3]=1
        with self.assertRaises(ValueError):model_asset._animation(a,1,list(range(6)))

if __name__=='__main__':unittest.main()
