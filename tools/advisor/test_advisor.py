import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import advisor


def candidate(result, name):
    return next(row for row in result['candidates'] if row['id'] == name)


class AdvisorTests(unittest.TestCase):
    def test_joint_dependencies_and_no_input_mutation(self):
        request = {'physics': ['joints']}
        original = copy.deepcopy(request)
        result = advisor.advise(request)
        self.assertEqual(result['request']['physics'], ['collision', 'joints', 'rigid_body'])
        self.assertEqual(len(result['dependency_notes']), 2)
        self.assertEqual(request, original)

    def test_pbr_does_not_require_ray_tracing(self):
        result = advisor.advise({'game': 'fps', 'material': 'pbr', 'shadows': 'shadow_map'})
        native = candidate(result, 'custom_windows')
        self.assertEqual(native['status'], 'existing_components')
        self.assertEqual(native['base_profile'], 'fps')
        self.assertNotIn('ray_acceleration.hardware_required', result['required_features'])

    def test_range_profile_only_for_its_actual_small_feature_set(self):
        minimal = candidate(advisor.advise({'game': 'range'}), 'custom_windows')
        shadow = candidate(advisor.advise({'game': 'range', 'shadows': 'shadow_map'}), 'custom_windows')
        self.assertEqual(minimal['base_profile'], 'range')
        self.assertEqual(shadow['base_profile'], 'all')

    def test_native_rigid_body_uses_selected_build(self):
        row = candidate(advisor.advise({'physics': ['rigid_body']}), 'custom_windows')
        self.assertEqual(row['status'], 'existing_components')
        self.assertNotIn('physics.rigid_body', row['missing_features'])
        self.assertIsNone(row['base_profile'])

    def test_local_source_authority_only_applies_to_card(self):
        card = candidate(advisor.advise({'game': 'card', 'network': 'authoritative'}), 'custom_windows')
        fps = candidate(advisor.advise({'game': 'fps', 'network': 'authoritative'}), 'custom_windows')
        self.assertNotIn('network.authoritative', card['missing_features'])
        self.assertIn('network.authoritative', fps['missing_features'])

    def test_web_excludes_native(self):
        result = advisor.advise({'target': 'web'})
        self.assertEqual(candidate(result, 'custom_windows')['status'], 'excluded')
        self.assertEqual(candidate(result, 'unity_windows')['status'], 'excluded')
        self.assertIn('three_web', result['comparison_candidates'])

    def test_windows_excludes_three_web(self):
        result = advisor.advise({'target': 'windows'})
        self.assertEqual(candidate(result, 'three_web')['status'], 'excluded')
        self.assertNotIn('three_web', result['comparison_candidates'])

    def test_hardware_ray_tracing_requires_target_gpu_validation(self):
        result = advisor.advise({'rendering': 'hybrid', 'reflections': 'ray_traced',
                                 'ray_acceleration': 'hardware_required'})
        self.assertEqual(candidate(result, 'custom_windows')['status'], 'requirements_unverified')
        self.assertEqual(candidate(result, 'unity_web')['status'], 'excluded')
        self.assertEqual(candidate(result, 'unity_windows')['status'], 'requirements_unverified')
        self.assertEqual(result['comparison_candidates'], [])

    def test_advanced_physics_requires_validation_in_external_candidates(self):
        result = advisor.advise({'physics': ['fluid', 'cloth']})
        self.assertEqual(candidate(result, 'godot_windows')['status'], 'requirements_unverified')
        self.assertEqual(candidate(result, 'three_web')['status'], 'requirements_unverified')

    def test_path_tracing_is_not_called_supported(self):
        result = advisor.advise({'rendering': 'path_tracing'})
        self.assertTrue(result['dependency_notes'])
        self.assertEqual(result['comparison_candidates'], [])

    def test_rejects_inconsistent_ray_selections(self):
        for value in [{'reflections': 'ray_traced'}, {'rendering': 'hybrid'},
                      {'ray_acceleration': 'hardware_required'}, {'ray_acceleration':'software'}]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                advisor.advise(value)

    def test_rejects_misspellings_duplicates_and_wrong_types(self):
        values = [[], {'physic': []}, {'physics': 'collision'}, {'physics': ['collision', 'collision']},
                  {'physics': [True]}, {'physics': ['magic']}, {'target': []}, {'width': True},
                  {'physics_hz': 0}, {'target_fps': float('nan')}, {'height': 16385}]
        for value in values:
            with self.subTest(value=value), self.assertRaises(ValueError):
                advisor.advise(value)

    def test_performance_is_target_only(self):
        result = advisor.advise({'width': 3840, 'height': 2160, 'target_fps': 144, 'physics_hz': 120})
        self.assertEqual(result['performance']['status'], 'not_measured')
        self.assertEqual(result['performance']['resolution'], [3840, 2160])
        self.assertEqual(result['performance']['physics_hz'], 120)

    def test_interactive_defaults_produce_valid_input(self):
        with patch('builtins.input', side_effect=[''] * (len(advisor.OPTIONS) + 5)), contextlib.redirect_stdout(io.StringIO()):
            request = advisor.interactive()
        self.assertEqual(advisor.advise(request)['request'], advisor.advise({})['request'])

    def test_cli_json_bom_input_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            config, out = folder / '入力.json', folder / 'report.json'
            config.write_text('{"physics":["rigid_body"]}', encoding='utf-8-sig')
            command = [sys.executable, '-B', str(Path(advisor.__file__).resolve()), '--config', str(config), '--json', '--out', str(out)]
            result = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            content = out.read_bytes()
            self.assertIn('physics.rigid_body', json.loads(content)['required_features'])
            result = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(out.read_bytes(), content)


if __name__ == '__main__':
    unittest.main()
