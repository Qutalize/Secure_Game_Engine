"""Run the poster's additional measurements serially, after the range benchmark."""
from pathlib import Path
import hashlib,json,random,re,statistics,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'output/benchmarks/poster-20261101'
from measure_selected_poster import measure
from run import percentile

def main():
 rows=json.loads((OUT/'range-comparison/runs.json').read_text())
 assert len(rows)==93 and all(r['status']=='ok' for r in rows),'Range benchmark must finish successfully first'
 package=ROOT/'output/packages/selected-dynamics-fbx-rt-signed-final'
 assert measure(package,OUT/'selected-demo',30)
 sys.path.insert(0,str(ROOT/'tools/advisor'))
 from verify_advanced import audit
 result=audit(package,OUT/'security-audit')
 print('Signature cases:',len(result['cases']),flush=True)
 probe=ROOT/'output/tools/distribution_probe.exe'
 cases={'range_signed':ROOT/'output/packages/range-signed-v2-final','selected_signed':package}
 samples={name:[] for name in cases};rng=random.Random(20261101)
 logs=OUT/'signature-timing';logs.mkdir(exist_ok=False)
 for i in range(31):
  names=list(cases);rng.shuffle(names)
  for name in names:
   r=subprocess.run([str(probe),str(cases[name])],cwd=OUT,capture_output=True,timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
   data=r.stdout+r.stderr;(logs/f'{i:03d}_{name}.log').write_bytes(data)
   m=re.search(rb'VERIFIED files=(\d+) bytes=(\d+) elapsed_ms=([0-9.]+)',data)
   samples[name].append({'warmup':i==0,'success':r.returncode==0 and bool(m),'exit_code':r.returncode,
                         'elapsed_ms':float(m[3]) if m else None})
   time.sleep(.05)
 report={'date':'2026-11-01','probe_sha256':hashlib.sha256(probe.read_bytes()).hexdigest(),
         'scope':'Existing 2026-10-04 probe, signature/manifest/files/open-handles verification only, excludes process launch; OS cache uncontrolled; 30 samples plus 1 warmup per case',
         'samples':samples,'summary':{}}
 for name,rs in samples.items():
  vals=[x['elapsed_ms'] for x in rs if x['success'] and not x['warmup']]
  report['summary'][name]={'valid':len(vals),'attempts':30,'median_ms':statistics.median(vals) if vals else None,'p95_ms':percentile(vals,95) if vals else None}
 (OUT/'signature-timing.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
 print('Signature timing:',report['summary'],flush=True)
 assert all(x['success'] for xs in samples.values() for x in xs)

if __name__=='__main__':main()