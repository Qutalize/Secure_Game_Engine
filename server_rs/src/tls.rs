//! Library TLS with a deadline on every underlying I/O, including fragmented records.
use rustls::{ServerConfig, ServerConnection, StreamOwned};
use std::{
    fs,
    io::{self, Read, Write},
    net::TcpStream,
    path::Path,
    sync::Arc,
    time::{Duration, Instant},
};

pub struct DeadlineSocket {
    socket: TcpStream,
    pub deadline: Instant,
}
impl DeadlineSocket {
    fn remaining(&self) -> io::Result<Duration> {
        self.deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "TLS/frame deadline exceeded"))
    }
}
impl Read for DeadlineSocket {
    fn read(&mut self, b: &mut [u8]) -> io::Result<usize> {
        self.socket
            .set_read_timeout(Some(self.remaining()?.max(Duration::from_millis(1))))?;
        self.socket.read(b)
    }
}
impl Write for DeadlineSocket {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.socket
            .set_write_timeout(Some(self.remaining()?.max(Duration::from_millis(1))))?;
        self.socket.write(b)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.socket.flush()
    }
}
pub type Stream = StreamOwned<ServerConnection, DeadlineSocket>;
pub fn accept(socket: TcpStream, config: Arc<ServerConfig>) -> io::Result<Stream> {
    socket.set_nonblocking(false)?;
    socket.set_nodelay(true)?;
    let mut conn = ServerConnection::new(config).map_err(io::Error::other)?;
    conn.set_buffer_limit(Some(64 * 1024));
    let mut stream = StreamOwned::new(
        conn,
        DeadlineSocket {
            socket,
            deadline: Instant::now() + Duration::from_secs(3),
        },
    );
    while stream.conn.is_handshaking() {
        stream.conn.complete_io(&mut stream.sock)?;
    }
    Ok(stream)
}
pub fn config(path: &Path) -> io::Result<Arc<ServerConfig>> {
    let cert = fs::read(path.join("server.pem"))?;
    let key = fs::read(path.join("server.key"))?;
    let certs = rustls_pemfile::certs(&mut &cert[..]).collect::<Result<Vec<_>, _>>()?;
    let key = rustls_pemfile::private_key(&mut &key[..])?
        .ok_or_else(|| io::Error::other("Missing TLS private key"))?;
    let mut config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .map_err(io::Error::other)?;
    // Application commands are never accepted as replayable early data.
    config.max_early_data_size = 0;
    config.send_tls13_tickets = 0;
    Ok(Arc::new(config))
}
pub fn generate(path: &Path) -> io::Result<()> {
    fs::create_dir(path)?; // Never overwrite an identity or rotate a pin silently.
    let signing_key = rcgen::KeyPair::generate().map_err(io::Error::other)?;
    let mut params =
        rcgen::CertificateParams::new(vec!["localhost".into()]).map_err(io::Error::other)?;
    params.not_before = time::OffsetDateTime::now_utc() - time::Duration::days(1);
    params.not_after = time::OffsetDateTime::now_utc() + time::Duration::days(90);
    let cert = params.self_signed(&signing_key).map_err(io::Error::other)?;
    fs::write(path.join("server.key"), signing_key.serialize_pem())?;
    fs::write(path.join("server.pem"), cert.pem())?;
    fs::write(path.join("server.der"), cert.der())?;
    println!("TLS identity created. Distribute only server.der to C++ clients; keep server.key on the server.");
    Ok(())
}
