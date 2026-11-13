"""Packaging boundary tests; uses the existing Mono build only as a negative input."""
from pathlib import Path
import unittest
import uuid
from package_unity import category,stage

ROOT=Path(__file__).resolve().parents[2]

class UnityPackageTests(unittest.TestCase):
    def test_runtime_files_are_kept(self):
        for name in ('RangeUnity.exe','UnityPlayer.dll','GameAssembly.dll','UnityCrashHandler64.exe','D3D12/D3D12Core.dll',
                     'RangeUnity_Data/il2cpp_data/Metadata/global-metadata.dat','RangeUnity_Data/StreamingAssets/range.json'):
            with self.subTest(name=name):self.assertEqual(category(Path(name)),'runtime')

    def test_debug_and_generated_sources_are_private(self):
        for name in ('GameAssembly.pdb','SymbolMap','RangeUnity_Data/il2cpp_data/Metadata/SymbolMap',
                     'RangeUnity_BackUpThisFolder_ButDontShipItWithYourGame/il2cppOutput/Assembly-CSharp.cpp',
                     'RangeUnity_BurstDebugInformation_DoNotShip/job.txt','RangeUnity_Data/Plugins/x86_64/job.pdb'):
            with self.subTest(name=name):self.assertEqual(category(Path(name)),'private-debug')

    def test_mono_or_unknown_files_are_rejected(self):
        for name in ('MonoBleedingEdge/EmbedRuntime/mono.dll','RangeUnity_Data/Managed/Assembly-CSharp.dll','extra.dll','unreviewed.txt'):
            with self.subTest(name=name),self.assertRaises(ValueError):category(Path(name))

    def test_actual_mono_cannot_be_relabelled(self):
        out=ROOT/'output/unity-package-tests'/uuid.uuid4().hex
        with self.assertRaisesRegex(ValueError,'completed IL2CPP'):
            stage(ROOT/'output/packages/range-unity-mono-control',out)
        self.assertFalse(out.exists())

if __name__=='__main__':unittest.main(verbosity=2)
