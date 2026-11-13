import os
from pathlib import Path
import tempfile
import unittest
import fbx_asset
from test_animation import fixture,deformed
from model_fixtures import fbx_ascii,fbx_binary

@unittest.skipUnless(os.name=='nt','The build-time FBX tool requires the selected Windows/MSVC environment')
class FbxTests(unittest.TestCase):
    def test_ascii_and_binary_match_glb_joint_poses(self):
        root=Path(__file__).resolve().parents[2];expected=fixture()
        for name in ('two-bone.fbx','two-bone-binary.fbx'):
            actual=fbx_asset.load(root/'assets/models/creator'/name,True,'Swing')
            for frame in (0,15,30,60,90,120):
                for vertex in range(6):
                    for a,b in zip(deformed(actual,frame,vertex),deformed(expected,frame,vertex)):self.assertAlmostEqual(a,b,places=6)
    def test_static_pose_and_selected_clip(self):
        root=Path(__file__).resolve().parents[2];path=root/'assets/models/creator/two-bone-binary.fbx'
        self.assertEqual(len(fbx_asset.load(path,False)['frames']),1)
        with self.assertRaises(ValueError):fbx_asset.load(path,True,'missing')
    def test_unicode_path_and_clip(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'骨モデル.fbx';path.write_bytes(fbx_binary(fbx_ascii().replace('Swing','動く')))
            actual=fbx_asset.load(path,True,'動く');self.assertEqual(actual['clip'],'動く')
    def test_reject_invalid_and_oversized_fbx(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'invalid.fbx'
            for data in (b'',b'not an FBX',b'0'*(8*1024*1024+1)):
                path.write_bytes(data)
                with self.assertRaises(ValueError):fbx_asset.load(path,False)

if __name__=='__main__':unittest.main()
