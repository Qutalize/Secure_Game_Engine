"""TLS boundary tests against our own server; captures contain encrypted records only."""
import test_integration as integration
from test_integration import SERVER, CLIENT, ROOT, request, receive, decode
import datetime
import select
import socket
import ssl
import subprocess
import threading
import time
import unittest
import warnings
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

class Proxy:
    def __init__(self,target):
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(1)
        self.address=self.listener.getsockname();self.target=target
        self.up=bytearray();self.down=bytearray();self.flip=threading.Event();self.stop=threading.Event()
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def run(self):
        sockets=[]
        try:
            self.listener.settimeout(4);client,_=self.listener.accept();sockets.append(client)
            server=socket.create_connection(self.target,timeout=4);sockets.append(server)
            pending=bytearray()
            while not self.stop.is_set():
                readable,_,_=select.select(sockets,[],[],.1)
                for origin in readable:
                    data=origin.recv(65536)
                    if not data:return
                    if origin is server:
                        self.down.extend(data);client.sendall(data)
                    else:
                        pending.extend(data)
                        while len(pending)>=5:
                            size=int.from_bytes(pending[3:5],'big')+5
                            if len(pending)<size:break
                            record=bytearray(pending[:size]);del pending[:size]
                            if self.flip.is_set() and record[0]==23:
                                record[-1]^=1;self.flip.clear()
                            self.up.extend(record);server.sendall(record)
        except (OSError,TimeoutError):pass
        finally:
            for s in sockets:s.close()
    def close(self):self.stop.set();self.listener.close();self.thread.join(timeout=5)

class TlsTests(integration.AuthorityTests):
    # Inherited protocol tests also run under TLS when this entire file is executed.
    def cpp(self,cert,filename='probe',address=None):
        address=address or self.address
        result=subprocess.run([str(CLIENT),'--host',address[0],'--port',str(address[1]),'--seat','0',
            '--token-file',str(self.keys/'seat0.key'),'--cert',str(cert),'--out',str(self.folder/(filename+'.wire')),
            '--frames','2','--seconds','8'],cwd=ROOT,capture_output=True,timeout=12,creationflags=subprocess.CREATE_NO_WINDOW)
        (self.folder/(filename+'.log')).write_bytes(result.stdout+result.stderr);return result
    def test_wrong_pin_cpp_is_rejected_before_seat_auth(self):
        self.start(humans=1);other=self.folder/'other-identity'
        subprocess.run([str(SERVER),'--init-tls',str(other)],check=True,capture_output=True,timeout=10)
        result=self.cpp(other/'server.der');self.assertEqual(result.returncode,1)
        self.assertIn(b'pin mismatch',result.stderr)
        peer,state=self.connect(0);self.assertEqual(state['round'],1)
    def test_plaintext_and_legacy_tls_are_rejected(self):
        self.start(humans=1)
        with socket.create_connection(self.address,timeout=4) as peer:
            peer.sendall(request(1,0,token=self.token(0)))
            with self.assertRaises((OSError,EOFError)): receive(peer)
        # TLS 1.1 cannot be negotiated (the Python build may reject it before sending).
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);context.check_hostname=False;context.verify_mode=ssl.CERT_NONE
        with warnings.catch_warnings():
            warnings.simplefilter('ignore',DeprecationWarning) # Deliberate legacy-protocol rejection test.
            context.maximum_version=ssl.TLSVersion.TLSv1_1
        with socket.create_connection(self.address,timeout=4) as raw:
            with self.assertRaises(ssl.SSLError):context.wrap_socket(raw,server_hostname='localhost')
        peer,state=self.connect(0);self.assertEqual(state['phase'],1)
    def test_encrypted_capture_and_tls12_compatibility(self):
        self.start(humans=1);proxy=Proxy(self.address)
        context=ssl.create_default_context(cafile=str(self.tls/'server.pem'));context.maximum_version=ssl.TLSVersion.TLSv1_2
        try:
            with context.wrap_socket(socket.create_connection(proxy.address,timeout=4),server_hostname='localhost') as peer:
                self.assertEqual(peer.version(),'TLSv1.2')
                peer.sendall(request(1,0,token=self.token(0)));state=decode(receive(peer));self.assertEqual(state['seat'],0)
                for _ in range(5):peer.sendall(request(2));decode(receive(peer))
        finally:proxy.close()
        (self.folder/'client-to-server.tls').write_bytes(proxy.up);(self.folder/'server-to-client.tls').write_bytes(proxy.down)
        self.assertGreater(len(proxy.up),100);self.assertGreater(len(proxy.down),100)
        self.assertNotIn(self.token(0),proxy.up);self.assertNotIn(b'ACAR\x01',proxy.up);self.assertNotIn(b'ACAR\x01',proxy.down)
    def test_modified_authenticated_record_is_rejected(self):
        self.start(humans=1);proxy=Proxy(self.address)
        context=ssl.create_default_context(cafile=str(self.tls/'server.pem'))
        try:
            with context.wrap_socket(socket.create_connection(proxy.address,timeout=4),server_hostname='localhost') as peer:
                proxy.flip.set();peer.sendall(request(1,0,token=self.token(0)))
                with self.assertRaises((OSError,EOFError)):receive(peer)
        finally:proxy.close()
        peer,state=self.connect(0);self.assertEqual(state['round'],1)
    def test_expired_pinned_certificate_is_rejected(self):
        self.start(humans=1)
        # A valid self-signature with expired dates: exact pin alone must not pass.
        key=ec.generate_private_key(ec.SECP256R1());name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')])
        now=datetime.datetime.now(datetime.timezone.utc)
        cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(days=3))
            .not_valid_after(now-datetime.timedelta(days=1)).add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),False)
            .sign(key,hashes.SHA256()))
        fixture=self.folder/'expired';fixture.mkdir()
        (fixture/'server.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        (fixture/'server.der').write_bytes(cert.public_bytes(serialization.Encoding.DER))
        (fixture/'server.key').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);address=listener.getsockname()
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(fixture/'server.pem',fixture/'server.key')
        observed=[]
        def peer_server():
            try:
                listener.settimeout(8);raw,_=listener.accept()
                with context.wrap_socket(raw,server_side=True) as peer:observed.append(peer.recv(100))
            except OSError:pass
            finally:listener.close()
        worker=threading.Thread(target=peer_server,daemon=True);worker.start()
        result=self.cpp(fixture/'server.der','expired-probe',address);worker.join(timeout=8)
        self.assertEqual(result.returncode,1);self.assertIn(b'expired',result.stderr)
        self.assertFalse(any(observed),'No seat token may be sent before certificate validation')

if __name__=='__main__':unittest.main(verbosity=2)
