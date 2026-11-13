import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import advisor
import build_runtime
from catalog import GENRES
from runtime_plan import emit

class RuntimeTests(unittest.TestCase):
    def test_feature_matrix(self):
        for physics,audio,material,shadow in itertools.product([[],['collision']],['none','basic'],['unlit','simple_lit','pbr'],['none','shadow_map']):
            config={'game':'tps','physics':physics,'audio':audio,'material':material,'shadows':shadow}
            if material=='unlit':
                config.update(lights='none',indirect_light='none')
            with self.subTest(config=config):
                plan=advisor.advise(config)['native_build']
                self.assertEqual(plan['status'],'blocked' if material=='unlit' and shadow!='none' else 'ready')
                self.assertEqual(plan['features']['audio'],audio=='basic')
                self.assertEqual(plan['features']['collision'],bool(physics))
                self.assertEqual(plan['features']['pbr'],material=='pbr')
                self.assertEqual('PbrPS' in plan['shaders'],material=='pbr')
                self.assertEqual('ShadowVS' in plan['shaders'],shadow=='shadow_map')
                self.assertNotIn('PbrSkinnedVS',plan['shaders'])

    def test_no_silent_fallback(self):
        requests=[{'physics':['joints']},{'audio':'spatial'},{'animation':'skeletal'},
                  {'network':'authoritative'},{'lights':'point_spot'},{'reflections':'environment_map'},
                  {'indirect_light':'baked'},{'target':'web'},{'game':'range'},
                  {'rendering':'hybrid','indirect_light':'ray_traced'},{'rendering':'path_tracing'}]
        for request in requests:
            with self.subTest(request=request):
                result=advisor.advise(request)
                self.assertEqual(result['native_build']['status'],'blocked')
                with patch('build_runtime.find_cmake') as finder:
                    with self.assertRaises(ValueError):
                        build_runtime.build(request)
                    finder.assert_not_called()

    def test_headless_excludes_rendering_and_shaders(self):
        result=advisor.advise({'rendering':'none','material':'unlit','lights':'none','indirect_light':'none','physics':[]})
        self.assertEqual(result['native_build']['status'],'ready')
        self.assertFalse(any(result['native_build']['features'].values()))
        self.assertEqual(result['native_build']['shaders'],[])
        self.assertEqual(advisor.advise({'rendering':'none'})['native_build']['status'],'blocked')

    def test_each_genre_is_an_explicit_starter(self):
        for game in GENRES:
            with self.subTest(game=game):
                result=advisor.advise({'game':game})
                self.assertEqual(result['native_build']['status'],'ready')
                self.assertEqual(result['game_guidance']['status'],'starter_template')
                self.assertTrue(result['game_guidance']['remaining_gameplay'])

    def test_packages_are_conditional_and_sourced(self):
        aim=advisor.advise({'game':'aim_training'})['recommended_packages']
        capture=advisor.advise({'game':'monster_capture','physics':['joints'],'network':'authoritative'})['recommended_packages']
        self.assertNotIn('jolt',{p['id'] for p in aim})
        self.assertNotIn('unity_netcode',{p['id'] for p in aim})
        self.assertTrue({'jolt','rapier','recast','cinemachine','unity_netcode'} <= {p['id'] for p in capture})
        for package in capture:
            self.assertTrue(package['source'].startswith('https://'))
            self.assertEqual(package['status'],'partially_integrated_rigid_boxes_only' if package['id']=='jolt' else 'optional_candidate_not_integrated')
        web=advisor.advise({'target':'web','game':'chase','physics':['rigid_body']})
        self.assertNotIn('jolt',{p['id'] for p in web['recommended_packages']})
        self.assertNotIn('recast',{p['id'] for p in web['recommended_packages']})

    def test_independent_rigid_ray_and_signature_choices(self):
        for rigid,ray,signed in itertools.product([False,True],repeat=3):
            config={'physics':['rigid_body'] if rigid else [],'distribution':'signed' if signed else 'unsigned'}
            if ray: config.update(rendering='hybrid',reflections='ray_traced',ray_acceleration='software')
            plan=advisor.advise(config)['native_build']
            self.assertEqual(plan['status'],'ready')
            for key,value in [('rigid_body',rigid),('ray_tracing',ray),('distribution_signature',signed)]:
                self.assertEqual(plan['features'][key],value)
        headless={'rendering':'none','material':'unlit','lights':'none','indirect_light':'none','reflections':'ray_traced'}
        self.assertEqual(advisor.advise(headless)['native_build']['status'],'blocked')

    def test_signed_selection_requires_explicit_matching_keys_before_build(self):
        with patch('build_runtime.find_cmake') as finder:
            with self.assertRaisesRegex(ValueError,'require --signing-key'):build_runtime.build({'distribution':'signed'})
            with self.assertRaisesRegex(ValueError,'unsigned'):build_runtime.build({},signing_key='unused')
            finder.assert_not_called()

    def test_ray_effects_have_independent_build_flags(self):
        for shadow,reflection in [('ray_traced','none'),('none','ray_traced'),('shadow_map','ray_traced')]:
            plan=advisor.advise({'rendering':'hybrid','shadows':shadow,'reflections':reflection})['native_build']
            self.assertEqual(plan['status'],'ready')
            self.assertEqual(plan['features']['rt_shadows'],shadow=='ray_traced')
            self.assertEqual(plan['features']['rt_reflections'],reflection=='ray_traced')
            self.assertEqual('ShadowVS' in plan['shaders'],shadow=='shadow_map')

    def test_wrong_signing_identity_rejected_before_cmake(self):
        try:
            from cryptography.hazmat.primitives import serialization
            from cryptography.hazmat.primitives.asymmetric import ec
        except ImportError:
            self.skipTest('cryptography is an explicit signing dependency')
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);private=folder/'private.pem';public=folder/'public.bin'
            key=ec.generate_private_key(ec.SECP256R1())
            private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
            n=ec.generate_private_key(ec.SECP256R1()).public_key().public_numbers()
            public.write_bytes(n.x.to_bytes(32,'big')+n.y.to_bytes(32,'big'))
            with patch('build_runtime.find_cmake') as finder:
                with self.assertRaisesRegex(ValueError,'does not match'):
                    build_runtime.build({'distribution':'signed'},signing_key=private,public_key=public)
                finder.assert_not_called()

    def test_generated_config_is_strict_and_consistent(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);config=folder/'input.json';out=folder/'generated'
            config.write_text(json.dumps({'game':'tps','material':'pbr','audio':'basic'}),encoding='utf-8-sig')
            emit(config,out)
            self.assertIn('#define ENGINE_AUDIO 1',(out/'SelectedFeatures.h').read_text())
            self.assertIn('set(RUNTIME_SHADOWS OFF)',(out/'features.cmake').read_text())
            self.assertNotIn('ShadowVS',json.loads((out/'runtime-plan.json').read_text(encoding='utf-8'))['shaders'])
            config.write_text('{"game":"x);message(FATAL_ERROR injected)"}')
            with self.assertRaises(ValueError): emit(config,out)

    def test_output_name_cannot_escape(self):
        for name in ('../parent','x/y','C:/temp','CON','aux','x;whoami',''):
            with self.subTest(name=name),self.assertRaises(ValueError): build_runtime.safe_name(name)
        self.assertEqual(build_runtime.safe_name('tps-demo-2'),'tps-demo-2')

    def test_cli_report_is_utf8_even_with_legacy_pipe_encoding(self):
        with tempfile.TemporaryDirectory() as folder:
            config=Path(folder)/'config.json';config.write_text('{"game":"monster_capture"}')
            env=dict(os.environ,PYTHONIOENCODING='cp932')
            done=subprocess.run([sys.executable,'-B',str(Path(advisor.__file__)),'--config',str(config),'--json'],
                                capture_output=True,timeout=10,env=env)
            self.assertEqual(done.returncode,0,done.stderr)
            self.assertEqual(json.loads(done.stdout.decode('utf-8'))['game_guidance']['label'],'モンスター捕獲・育成')

    def test_cli_invalid_config_does_not_build(self):
        with tempfile.TemporaryDirectory() as folder:
            config=Path(folder)/'bad.json';config.write_text('{"physics":["fluid"]}')
            done=subprocess.run([sys.executable,'-B',str(Path(build_runtime.__file__)),'--config',str(config)],capture_output=True,timeout=10)
            self.assertEqual(done.returncode,2)
            self.assertNotIn(b'cmake.exe',done.stdout)

if __name__=='__main__': unittest.main()
