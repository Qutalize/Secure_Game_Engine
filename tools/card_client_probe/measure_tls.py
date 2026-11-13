"""C++ Schannel request/response timing on loopback; no server audit writes and no rendering."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time
import test_integration as fixture

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--runs',type=int,default=7);a=p.parse_args()
    if a.runs<3:p.error('At least 3 runs required')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);rows=[]
    for i in range(a.runs):
        fixture.RUN=out/'server-fixtures'/f'{i:02d}'
        case=fixture.AuthorityTests('test_recipient_frames_and_credentials');case.setUp()
        try:
            case.start(humans=1,audit=False);timing=out/f'{i:02d}.csv'
            with (out/f'{i:02d}.log').open('wb') as log:
                subprocess.run([str(fixture.CLIENT),'--host',case.address[0],'--port',str(case.address[1]),'--seat','0',
                    '--token-file',str(case.keys/'seat0.key'),'--cert',str(case.tls/'server.der'),'--out',str(out/f'{i:02d}.wire'),
                    '--timings',str(timing),'--frames','101','--seconds','15'],check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            with timing.open(newline='',encoding='utf8') as f:samples=list(csv.DictReader(f))
            values=[float(r['elapsed_ms']) for r in samples[1:]]
            if len(values)!=100:raise ValueError('Incomplete timing samples')
            values.sort();row=dict(run=i,connect_tls_hello_ms=float(samples[0]['elapsed_ms']),poll_count=len(values),
                poll_p50_ms=statistics.median(values),poll_p95_ms=values[94]*.95+values[95]*.05,poll_max_ms=max(values))
            rows.append(row);print(row,flush=True)
        finally:case.tearDown()
    result=dict(host=platform.node(),os=platform.platform(),probe_sha256=hashlib.sha256(fixture.CLIENT.read_bytes()).hexdigest(),
        scope='Loopback; new TLS connection + seat auth and 100 existing-connection POLLs per run; 20ms pacing excluded; no server audit; not WAN or input latency',
        rows=rows,median_connect_ms=statistics.median(r['connect_tls_hello_ms'] for r in rows),
        median_poll_p50_ms=statistics.median(r['poll_p50_ms'] for r in rows),median_poll_p95_ms=statistics.median(r['poll_p95_ms'] for r in rows))
    (out/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf8')
if __name__=='__main__':main()
