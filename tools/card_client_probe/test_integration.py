"""Real Rust server / Python adversarial peers / C++ client tests; local test children only."""
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import time
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[2]
SERVER=ROOT/'server_rs/target/release/card_authority.exe'
CLIENT=ROOT/'build-event/Release/card_client_probe.exe'
GAME=Path(os.environ.get('CARD_TEST_GAME',str(ROOT/'build-event/Release/GameEngine.exe')))
RUN=ROOT/'output/card-authority-tests'/uuid.uuid4().hex

def request(kind, arg=0, sequence=0, revision=0, token=bytes(16)):
    return struct.pack('<4sBBBBQQ16s',b'ACAR',1,kind,arg,0,sequence,revision,token)

def receive(peer):
    raw=b''
    while len(raw)<64:
        part=peer.recv(64-len(raw))
        if not part: raise EOFError('connection closed')
        raw+=part
    return raw

def decode(raw):
    assert len(raw)==64 and raw[:6]==b'ACAR\x01\x80'
    error,seat=raw[6:8]
    seq,rev,round_id=struct.unpack_from('<QQI',raw,8)
    turn,victim,loser,own_count=raw[28:32]
    counts=list(raw[32:36]);discarded,phase,humans=raw[36:39]
    assert 0<=seat<4 and own_count<=14 and own_count==counts[seat]
    assert raw[39]==0 and raw[40+own_count:]==bytes(24-own_count)
    own=list(raw[40:40+own_count]);assert len(set(own))==len(own) and all(c<=52 for c in own)
    if round_id: assert sum(counts)+discarded*2==53
    return dict(error=error,seat=seat,seq=seq,rev=rev,round=round_id,turn=turn,victim=victim,loser=loser,
                own=own,counts=counts,discarded=discarded,phase=phase,humans=humans)

class AuthorityTests(unittest.TestCase):
    def setUp(self):
        self.folder=RUN/self._testMethodName
        self.folder.mkdir(parents=True,exist_ok=False)
        self.children=[];self.peers=[];self.logs=[]

    def tearDown(self):
        for peer in self.peers: peer.close()
        for child in self.children:
            if child.poll() is None: child.terminate()
            child.wait(timeout=5)
        for log in self.logs: log.close()

    def start(self,humans=4,bot_ms=10,audit=True):
        keys=self.folder/'keys';ready=self.folder/'ready.txt'
        self.tls=self.folder/'tls'
        subprocess.run([str(SERVER),'--init-tls',str(self.tls)],check=True,capture_output=True,timeout=10)
        log=(self.folder/'server.log').open('wb');self.logs.append(log)
        command=[str(SERVER),'--bind','127.0.0.1:0','--humans',str(humans),
            '--keys-dir',str(keys),'--tls-dir',str(self.tls),'--ready-file',str(ready),'--bot-delay-ms',str(bot_ms),'--run-seconds','180']
        if audit:command.extend(['--audit-dir',str(self.folder/'sent')])
        child=subprocess.Popen(command,
            cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        self.children.append(child)
        deadline=time.monotonic()+8
        endpoint=''
        while ':' not in endpoint:
            if child.poll() is not None: self.fail('Server failed; see '+str(self.folder/'server.log'))
            if time.monotonic()>deadline: self.fail('Server not ready')
            if ready.exists():endpoint=ready.read_text()
            time.sleep(.02)
        host,port=endpoint.split(':');self.address=(host,int(port));self.keys=keys
        return child

    def peer(self):
        context=ssl.create_default_context(cafile=str(self.tls/'server.pem'))
        context.minimum_version=ssl.TLSVersion.TLSv1_2
        peer=context.wrap_socket(socket.create_connection(self.address,timeout=3),server_hostname='localhost')
        peer.settimeout(3);self.peers.append(peer);return peer

    def token(self,seat): return bytes.fromhex((self.keys/f'seat{seat}.key').read_text())

    def connect(self,seat):
        peer=self.peer();peer.sendall(request(1,seat,token=self.token(seat)))
        state=decode(receive(peer));self.assertEqual(state['seat'],seat);return peer,state

    def poll(self,peer): peer.sendall(request(2));return decode(receive(peer))

    def all_seats(self):
        peers=[self.connect(s)[0] for s in range(4)]
        return peers,[self.poll(p) for p in peers]

    def test_recipient_frames_and_credentials(self):
        server=self.start();peers,states=self.all_seats()
        self.assertTrue(all(s['phase']==1 for s in states))
        self.assertEqual(len({s['rev'] for s in states}),1)
        hands=[c for s in states for c in s['own']]
        self.assertEqual(len(hands),len(set(hands)))
        for seat in range(4):
            peers[seat].sendall(request(2));raw=receive(peers[seat]);decode(raw)
            (self.folder/f'seat{seat}.response').write_bytes(raw)
            self.assertNotIn(self.token(seat),raw)
        for arg,token in [(1,self.token(0)),(0,bytes(16)),(0,self.token(0))]:
            peer=self.peer();peer.sendall(request(1,arg,token=token))
            with self.assertRaises((EOFError,ConnectionError)): receive(peer)
            peer.close()
        self.assertIsNone(server.poll());self.assertEqual(self.poll(peers[0])['phase'],1)

    def test_actions_are_authoritative_and_replay_safe(self):
        self.start();peers,states=self.all_seats();seat=states[0]['turn'];actor=peers[seat];s=states[seat]
        before=[state['own'] for state in states]
        wrong=(seat+1)%4
        peers[wrong].sendall(request(3,0,states[wrong]['seq'],states[wrong]['rev']))
        self.assertEqual(decode(receive(peers[wrong]))['error'],1)
        actor.sendall(request(3,255,s['seq'],s['rev']));s=decode(receive(actor));self.assertEqual(s['error'],2)
        actor.sendall(request(3,0,s['seq'],s['rev']-1));s=decode(receive(actor));self.assertEqual(s['error'],3)
        self.assertEqual([self.poll(p)['own'] for p in peers],before)
        command=request(3,0,s['seq'],s['rev']);actor.sendall(command);after=decode(receive(actor));self.assertEqual(after['error'],0)
        hands=[self.poll(p)['own'] for p in peers]
        actor.sendall(command);duplicate=decode(receive(actor));self.assertEqual(duplicate['error'],4)
        self.assertEqual(duplicate['rev'],after['rev']);self.assertEqual([self.poll(p)['own'] for p in peers],hands)

    def test_disconnect_pauses_and_reauth_resumes_without_redeal(self):
        self.start();peers,states=self.all_seats();peers[2].close()
        deadline=time.monotonic()+2
        while (paused:=self.poll(peers[0]))['phase']!=0:
            self.assertLess(time.monotonic(),deadline);time.sleep(.04)
        time.sleep(.2);self.assertEqual(self.poll(peers[0])['rev'],paused['rev'])
        peer,state=self.connect(2)
        self.assertEqual(state['own'],states[2]['own']);self.assertEqual(state['round'],1)
        self.assertEqual(state['seq'],states[2]['seq']);self.assertEqual(state['phase'],1)
        # A second HELLO cannot change the identity on an authenticated connection.
        peer.sendall(request(1,1,token=self.token(1)))
        with self.assertRaises((EOFError,ConnectionError)): receive(peer)

    def test_bad_frames_are_bounded_and_server_survives(self):
        server=self.start(humans=1)
        for kind in [0,5,128,255]:
            peer=self.peer();peer.sendall(request(kind))
            with self.assertRaises((EOFError,ConnectionError)): receive(peer)
            peer.close()
        partial=self.peer();began=time.monotonic();partial.sendall(request(1,0,token=self.token(0))[:39])
        with self.assertRaises((EOFError,ConnectionError)): receive(partial)
        self.assertLess(time.monotonic()-began,3)
        # Legitimate connections heartbeat every 80 ms; idle peers intentionally expire at 2 s.
        peer,_=self.connect(0)
        self.assertIsNone(server.poll());self.assertEqual(self.poll(peer)['phase'],1)

    def test_cpp_clients_complete_two_rounds(self):
        self.start(humans=2)
        players=[]
        for seat in range(2):
            log=(self.folder/f'client{seat}.log').open('wb');self.logs.append(log)
            child=subprocess.Popen([str(CLIENT),'--host',self.address[0],'--port',str(self.address[1]),
                '--seat',str(seat),'--token-file',str(self.keys/f'seat{seat}.key'),'--cert',str(self.tls/'server.der'),'--out',str(self.folder/f'client{seat}.wire'),
                '--autoplay','--rounds','2','--seconds','90'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            self.children.append(child);players.append(child)
        for child in players: self.assertEqual(child.wait(timeout=100),0)
        for seat in range(2):
            data=(self.folder/f'client{seat}.wire').read_bytes();self.assertEqual(len(data)%64,0)
            deadline=time.monotonic()+2
            while len((self.folder/'sent'/f'seat{seat}.wire').read_bytes())<len(data):
                self.assertLess(time.monotonic(),deadline);time.sleep(.02)
            self.assertEqual(data,(self.folder/'sent'/f'seat{seat}.wire').read_bytes())
            snapshots=[decode(data[i:i+64]) for i in range(0,len(data),64)]
            self.assertTrue(all(s['seat']==seat for s in snapshots))
            self.assertEqual(snapshots[-1]['phase'],2);self.assertEqual(snapshots[-1]['round'],2)
            self.assertTrue(any(s['round']==1 for s in snapshots))

    def test_graphical_card_uses_remote_state(self):
        self.start(humans=1,bot_ms=20)
        path=self.folder/'game.log';log=path.open('wb');self.logs.append(log)
        child=subprocess.Popen([str(GAME),'--mode','card','--card-server',f'{self.address[0]}:{self.address[1]}',
            '--card-seat','0','--card-token-file',str(self.keys/'seat0.key'),'--card-cert',str(self.tls/'server.der'),'--card-autoplay',
            '--security','authority,private-hands','--security-report',str(self.folder/'security.csv')],
            cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        self.children.append(child);self.assertEqual(child.wait(timeout=90),0);log.flush()
        text=path.read_text(encoding='utf8',errors='replace')
        self.assertIn('CARD REMOTE COMPLETE',text);self.assertIn('CARD VIEW',text)
        self.assertNotIn('card.opponentHand',text);self.assertNotIn('Watchdog] 整合性監視スレッド開始',text)
        for line in (self.folder/'security.csv').read_text().splitlines()[1:]: self.assertEqual(line.split(',')[1],'0')

if __name__=='__main__':
    print('Evidence directory:',RUN,flush=True)
    unittest.main(verbosity=2)
