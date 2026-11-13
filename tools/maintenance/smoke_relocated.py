"""Run an already copied workspace's Players; keep reports outside signed packages."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'comparison'))
from compare_traces import compare, read


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    project = args.project.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'project': str(project), 'scope': 'same PC, relocated path; not a separate-PC test', 'runs': [], 'successful': False}

    def run(name, command):
        with (out / (name + '-stdout.log')).open('wb') as stream:
            result = subprocess.run([str(x) for x in command], cwd=out, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        report['runs'].append({'name': name, 'exit_code': result.returncode})
        if result.returncode:
            raise RuntimeError(f'{name}: exit {result.returncode}; inspect log')
        print(name + ': ok', flush=True)

    try:
        reference = out / 'reference.csv'
        run('reference', [project / 'output/tools/range_trace.exe', project / 'comparison/data/range.json',
                          project / 'comparison/data/victory.json', reference])
        for name in ['range-signed-v2-final', 'game-signed-v2-final', 'rebuilt-range']:
            package = project / 'output/packages' / name
            exe = package / ('GameEngine.exe' if name.startswith('game-') else 'RangeGame.exe')
            if name != 'rebuilt-range':
                run(name + '-verify', [exe, '--verify-distribution'])
            trace = out / (name + '.csv')
            run(name, [exe, '--mode', 'range', '--replay', package / 'data/victory.json',
                       '--state-out', trace, '--exit-after-replay'])
            report[name] = compare(read(reference), read(trace))
            report[name]['exe_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
        for name in ['range-unity-il2cpp-protected', 'range-unity-mono-control']:
            package = project / 'output/packages' / name
            trace = out / (name + '.csv')
            run(name, [package / 'RangeUnity.exe', '-force-d3d11', '-logFile', out / (name + '.log'),
                       '--replay', package / 'RangeUnity_Data/StreamingAssets/victory.json',
                       '--state-out', trace, '--exit-after-replay'])
            report[name] = compare(read(reference), read(trace))
        run('security-unit', [project / 'output/tools/security_test.exe'])
        run('input-unit', [project / 'output/tools/input_test.exe'])
        run('server-init', [project / 'output/tools/card_authority.exe', '--init-tls', out / 'temporary-tls'])
        report['successful'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
