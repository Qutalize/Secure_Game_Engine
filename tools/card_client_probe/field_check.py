"""Collect TLS card evidence on separate Windows PCs; never copies private server files to clients."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def metadata(exe):
    identity=platform.node();source='hostname'
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'SOFTWARE\Microsoft\Cryptography',0,winreg.KEY_READ|winreg.KEY_WOW64_64KEY) as key:
            identity=winreg.QueryValueEx(key,'MachineGuid')[0];source='Windows MachineGuid SHA256'
    except (ImportError,OSError):pass
    return dict(host=platform.node(),host_id=hashlib.sha256(identity.encode()).hexdigest(),host_id_source=source,
                os=platform.platform(),exe_sha256=sha(exe),started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
def save(path,data):path.write_text(json.dumps(data,indent=2),encoding='utf8')
def frames(path,seat):
    data=path.read_bytes()
    if not data or len(data)%64:raise ValueError(f'Incomplete reply file: {path}')
    for i in range(0,len(data),64):
        b=data[i:i+64];count=b[31];counts=b[32:36];round_id=struct.unpack_from('<I',b,24)[0]
        if b[:6]!=b'ACAR\x01\x80' or b[7]!=seat or count>14 or count!=counts[seat] or b[39]!=0 or b[40+count:]!=bytes(24-count):
            raise ValueError(f'Invalid recipient/padding at frame {i//64}')
        own=b[40:40+count]
        if len(set(own))!=len(own) or any(c>52 for c in own) or (round_id and sum(counts)+b[36]*2!=53):
            raise ValueError('Invalid card state')
    return data
def verify(server,clients):
    sm=json.loads((server/'server.json').read_text(encoding='utf8'));rows=[];seen=set()
    for client in clients:
        cm=json.loads((client/'client.json').read_text(encoding='utf8'));seat=cm['seat']
        if not 0<=seat<4 or seat in seen:raise ValueError('Invalid or duplicate seat')
        seen.add(seat)
        actual=frames(client/'received.wire',seat);sent=frames(server/'sent'/f'seat{seat}.wire',seat)
        # Each new field run has exactly one connection per seat: byte-for-byte equality is required.
        if actual!=sent:raise ValueError(f'Seat {seat} send/receive mismatch; preserve both records')
        if cm['exit_code']!=0 or cm['certificate_sha256']!=sm['certificate_sha256']:raise ValueError('Client failed or wrong run certificate')
        if actual[-27]!=2:raise ValueError('No completed round in final snapshot')
        rows.append(dict(seat=seat,frames=len(actual)//64,sha256=hashlib.sha256(actual).hexdigest(),
                         distinct_from_server=cm['host_id']!=sm['host_id'],host_id=cm['host_id']))
    if len(rows)!=sm['humans'] or seen!=set(range(sm['humans'])):raise ValueError('Collect every human seat')
    distinct=all(r['distinct_from_server'] for r in rows) and len({r['host_id'] for r in rows})==len(rows)
    return dict(recipient_transcripts_match=True,distinct_host_evidence=distinct,clients=rows,
                limitation='Host metadata is self-reported, not remote attestation. Confirm physical PCs with the operator. Same-host results are local rehearsal only. Server snapshot noninterference is separately unit-tested.')
def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    s=sub.add_parser('serve');s.add_argument('--server',type=Path,required=True);s.add_argument('--out',type=Path,required=True)
    s.add_argument('--bind',default='0.0.0.0:7778');s.add_argument('--humans',type=int,choices=range(1,5),default=2)
    s.add_argument('--seconds',type=int,default=600);s.add_argument('--bot-ms',type=int,default=20)
    c=sub.add_parser('client');c.add_argument('--probe',type=Path,required=True);c.add_argument('--host',required=True);c.add_argument('--port',type=int,default=7778)
    c.add_argument('--seat',type=int,choices=range(4),required=True);c.add_argument('--token-file',type=Path,required=True);c.add_argument('--cert',type=Path,required=True)
    c.add_argument('--out',type=Path,required=True);c.add_argument('--rounds',type=int,default=2);c.add_argument('--seconds',type=int,default=180)
    v=sub.add_parser('verify');v.add_argument('--server',type=Path,required=True);v.add_argument('--clients',type=Path,nargs='+',required=True);v.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.action=='verify':
        report=verify(a.server,a.clients);save(a.out,report);print(json.dumps(report,indent=2))
        return 0 if report['distinct_host_evidence'] else 2
    a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
    if a.action=='serve':
        exe=a.server.resolve();tls=a.out/'tls'
        subprocess.run([str(exe),'--init-tls',str(tls)],check=True)
        m=metadata(exe);m.update(humans=a.humans,certificate_sha256=sha(tls/'server.der'));save(a.out/'server.json',m)
        command=[str(exe),'--bind',a.bind,'--humans',str(a.humans),'--keys-dir',str(a.out/'keys'),'--tls-dir',str(tls),
                 '--audit-dir',str(a.out/'sent'),'--ready-file',str(a.out/'ready.txt'),'--run-seconds',str(a.seconds),'--bot-delay-ms',str(a.bot_ms)]
        print('Keep tls/server.key, keys for other seats, and sent/ on the trusted server. Copy only server.der and the intended seat key.',flush=True)
        logname='server.log'
    else:
        exe=a.probe.resolve();m=metadata(exe);m.update(seat=a.seat,certificate_sha256=sha(a.cert),endpoint=f'{a.host}:{a.port}')
        save(a.out/'client.json',m)
        command=[str(exe),'--host',a.host,'--port',str(a.port),'--seat',str(a.seat),'--token-file',str(a.token_file.resolve()),
                 '--cert',str(a.cert.resolve()),'--out',str(a.out/'received.wire'),'--autoplay','--rounds',str(a.rounds),'--seconds',str(a.seconds)]
        logname='client.log'
    with (a.out/logname).open('wb') as log:
        child=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:code=child.wait(timeout=a.seconds+10 if a.seconds else None)
        except BaseException:child.terminate();child.wait(timeout=5);raise
    m['exit_code']=code;m['ended_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    save(a.out/('server.json' if a.action=='serve' else 'client.json'),m);print(a.action,'exit',code,flush=True);return code
if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError) as error:print(error,file=sys.stderr);raise SystemExit(1)
