"""Configuration errors must fail before starting the renderer or legacy server."""
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]
GAME=ROOT/'build-event/Release/GameEngine.exe'

class ConfigurationTests(unittest.TestCase):
    def test_invalid_configurations_exit_with_configuration_error(self):
        cases=[
            ['--security','shufffle'],
            ['--security','none','--security','watchdog'],
            ['--mode','card','--security','depth-guard'],
            ['--mode','fps','--security','private-hands'],
            ['--mode','range','--security','watchdog'],
            ['--mode','card','--card-server','127.0.0.1:7778','--security','none'],
            ['--mode','card','--card-seat','0'],
            ['--server','--security','none'],
            ['--security','none','--server'],
            ['--server','65536'],
            ['--mode'],
            ['--security-report'],
            ['--securty','none'],
        ]
        for args in cases:
            with self.subTest(args=args):
                process=subprocess.run([str(GAME),*args],cwd=ROOT,capture_output=True,timeout=5,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                self.assertEqual(process.returncode,2,process.stdout+process.stderr)
                self.assertNotIn(b'SECURITY START',process.stdout)

if __name__=='__main__':unittest.main(verbosity=2)
