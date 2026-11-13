"""Exercise the shipped multi-PC collection commands locally without claiming separate PCs."""
import json
from pathlib import Path
import subprocess
import sys
import time
import unittest
import uuid
import field_check

ROOT=Path(__file__).resolve().parents[2]

class FieldTests(unittest.TestCase):
    def test_real_collectors_and_same_host_rejection(self):
        out=ROOT/'output/card-field-tests'/uuid.uuid4().hex;out.mkdir(parents=True)
        tool=ROOT/'tools/card_client_probe/field_check.py';server=out/'server';children=[]
        def start(args):
            child=subprocess.Popen([sys.executable,str(tool),*map(str,args)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW);children.append(child);return child
        try:
            authority=start(['serve','--server',ROOT/'server_rs/target/release/card_authority.exe','--bind','127.0.0.1:0',
                             '--humans',2,'--seconds',30,'--out',server])
            deadline=time.monotonic()+8
            endpoint=''
            while ':' not in endpoint:
                self.assertIsNone(authority.poll());self.assertLess(time.monotonic(),deadline);time.sleep(.02)
                if (server/'ready.txt').exists():endpoint=(server/'ready.txt').read_text()
            host,port=endpoint.split(':');players=[]
            for seat in range(2):
                players.append(start(['client','--probe',ROOT/'build-event/Release/card_client_probe.exe','--host',host,'--port',port,
                    '--seat',seat,'--token-file',server/'keys'/f'seat{seat}.key','--cert',server/'tls/server.der',
                    '--out',out/f'client{seat}','--rounds',2,'--seconds',20]))
            for player in players:
                text,_=player.communicate(timeout=25);self.assertEqual(player.returncode,0,text.decode(errors='replace'))
            args=['verify','--server',server,'--clients',out/'client0',out/'client1','--out',out/'report.json']
            result=subprocess.run([sys.executable,str(tool),*map(str,args)],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,2,result.stderr.decode(errors='replace'))
            report=json.loads((out/'report.json').read_text());self.assertTrue(report['recipient_transcripts_match'])
            self.assertFalse(report['distinct_host_evidence'])
            damaged=out/'client0/received.wire';original=damaged.read_bytes();damage=bytearray(original);damage[7]=1
            damaged.write_bytes(damage)
            with self.assertRaisesRegex(ValueError,'recipient'):field_check.verify(server,[out/'client0',out/'client1'])
            damaged.write_bytes(original)
            print('Field evidence:',out,flush=True)
        finally:
            for child in children:
                if child.poll() is None:child.terminate()
                child.communicate(timeout=5)

if __name__=='__main__':unittest.main(verbosity=2)
