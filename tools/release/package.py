"""Stage a protected Release into a new folder, scrub known metadata, then audit it."""
import argparse
import json
from pathlib import Path
import subprocess
from audit_package import audit
from scrub_pe import scrub

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--cmake',default='cmake');p.add_argument('--signing-key',type=Path);a=p.parse_args()
    cache=(a.build/'CMakeCache.txt').read_text(encoding='utf8')
    if 'PROTECT_DISTRIBUTION:BOOL=ON' not in cache:p.error('Use a PROTECT_DISTRIBUTION build')
    signed='VERIFY_DISTRIBUTION:BOOL=ON' in cache
    if signed and not a.signing_key:p.error('This build requires --signing-key stored outside the package')
    if a.signing_key and not signed:p.error('A signature without an enforcing build is not supported')
    if a.out.exists():p.error('Use a new output directory')
    subprocess.run([a.cmake,'--install',str(a.build),'--config','Release','--prefix',str(a.out)],check=True)
    for exe in a.out.glob('*.exe'):scrub(exe)
    if signed:
        from sign_distribution import sign
        public=next(line.split('=',1)[1] for line in cache.splitlines() if line.startswith('DISTRIBUTION_PUBLIC_KEY_FILE:FILEPATH='))
        sign(a.out,a.signing_key,Path(public))
    report=audit(a.out);report_path=a.out.with_name(a.out.name+'-audit.json')
    report_path.write_text(json.dumps(report,indent=2),encoding='utf8')
    if report['problems']:raise RuntimeError('Package audit failed; inspect '+str(report_path))
    if signed:
        exe=next(a.out.glob('*.exe')).resolve()
        result=subprocess.run([str(exe),'--verify-distribution'],capture_output=True,timeout=15,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        report['distribution_verification']={'exit_code':result.returncode,'enforced':result.returncode==0}
        report_path.write_text(json.dumps(report,indent=2),encoding='utf8')
        result.check_returncode()
    print('Package validated:',a.out)
if __name__=='__main__':main()
