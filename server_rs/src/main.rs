#![forbid(unsafe_code)]
mod audit;
mod tls;
use card_authority::*;
use std::{
    fs,
    io::{self, Read, Write},
    net::{TcpListener, TcpStream},
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}
fn keys(directory: &PathBuf) -> io::Result<[[u8; 16]; 4]> {
    // Each seat file is a credential. Distribute only the intended seat file to its owner.
    // A new directory generates keys; an existing directory must contain all four valid files.
    let create = match fs::create_dir(directory) {
        Ok(()) => true,
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => false,
        Err(e) => return Err(e),
    };
    let mut keys = [[0u8; 16]; 4];
    for seat in 0..4 {
        let path = directory.join(format!("seat{seat}.key"));
        if create {
            getrandom::fill(&mut keys[seat]).map_err(|e| io::Error::other(e.to_string()))?;
            let hex: String = keys[seat].iter().map(|b| format!("{b:02x}")).collect();
            let mut file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)?;
            file.write_all(hex.as_bytes())?;
        } else {
            let bytes = fs::read(&path)?;
            if bytes.len() != 32 || !bytes.iter().all(u8::is_ascii_hexdigit) {
                return Err(invalid("Invalid seat credential file"));
            }
            for i in 0..16 {
                keys[seat][i] =
                    u8::from_str_radix(std::str::from_utf8(&bytes[i * 2..i * 2 + 2]).unwrap(), 16)
                        .map_err(|_| invalid("Invalid key"))?;
            }
        }
        if keys[seat] == [0; 16] || keys[..seat].contains(&keys[seat]) {
            return Err(invalid("Seat keys must be nonzero and distinct"));
        }
    }
    Ok(keys)
}
fn token_matches(a: &[u8; 16], b: &[u8; 16]) -> bool {
    a.iter().zip(b).fold(0u8, |diff, (x, y)| diff | (x ^ y)) == 0
}
fn receive(stream: &mut tls::Stream) -> io::Result<Request> {
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut b = [0; REQUEST_SIZE];
    let mut received = 0;
    while received < b.len() {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(|| invalid("Frame deadline exceeded"))?;
        stream.sock.deadline = deadline;
        let _ = remaining;
        match stream.read(&mut b[received..]) {
            Ok(0) => return Err(io::ErrorKind::UnexpectedEof.into()),
            Ok(n) => received += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Request::decode(&b).map_err(invalid)
}
struct Claim {
    game: Arc<Mutex<Game>>,
    seat: u8,
}
impl Drop for Claim {
    fn drop(&mut self) {
        self.game.lock().unwrap().release(self.seat);
    }
}
struct Connection(Arc<AtomicUsize>);
impl Drop for Connection {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::Relaxed);
    }
}
fn serve(
    socket: TcpStream,
    game: Arc<Mutex<Game>>,
    keys: Arc<[[u8; 16]; 4]>,
    stop: Arc<AtomicBool>,
    config: Arc<rustls::ServerConfig>,
    audit: Option<Arc<audit::Audit>>,
) -> io::Result<()> {
    // Windows inherits the listener's nonblocking mode on accepted sockets.
    // Worker reads use a bounded blocking deadline, independently of accept().
    let mut stream = tls::accept(socket, config)?;
    let hello = receive(&mut stream)?;
    if hello.kind != HELLO || !token_matches(&hello.token, &keys[hello.arg as usize]) {
        return Err(invalid("Authentication failed")); // Never serialize a hand before authentication.
    }
    let seat = hello.arg;
    let snapshot = {
        let mut state = game.lock().unwrap();
        if !state.claim(seat, &mut OsRandom)? {
            return Err(invalid("Seat unavailable"));
        }
        state.snapshot(seat, 0)
    };
    let _claim = Claim {
        game: game.clone(),
        seat,
    };
    stream.sock.deadline = Instant::now() + Duration::from_secs(2);
    stream.write_all(&snapshot)?;
    stream.flush()?;
    if let Some(audit) = &audit {
        audit.record(seat, &snapshot)?;
    }
    println!("CARD CONNECT seat={seat}");
    let mut window = Instant::now();
    let mut requests = 0u32;
    while !stop.load(Ordering::Relaxed) {
        let request = receive(&mut stream)?;
        if request.kind == HELLO {
            return Err(invalid("Connection seat cannot change"));
        }
        if window.elapsed() > Duration::from_secs(1) {
            window = Instant::now();
            requests = 0;
        }
        requests += 1;
        if requests > 100 {
            return Err(invalid("Request rate exceeded"));
        }
        let reply = {
            let mut state = game.lock().unwrap();
            let error = state.command(seat, &request, &mut OsRandom)?;
            state.snapshot(seat, error)
        };
        stream.sock.deadline = Instant::now() + Duration::from_secs(2);
        stream.write_all(&reply)?;
        stream.flush()?;
        if let Some(audit) = &audit {
            audit.record(seat, &reply)?;
        }
    }
    Ok(())
}
fn run() -> io::Result<()> {
    let initial: Vec<_> = std::env::args().collect();
    if initial.len() == 3 && initial[1] == "--init-tls" {
        return tls::generate(std::path::Path::new(&initial[2]));
    }
    let mut bind = "127.0.0.1:7778".to_string();
    let mut humans = 1u8;
    let mut key_dir = None;
    let mut tls_dir = None;
    let mut audit_dir = None;
    let mut ready_file = None;
    let mut duration = 0u64;
    let mut bot_ms = 700u64;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| invalid("Missing argument value"))?;
        match arg.as_str() {
            "--bind" => bind = value,
            "--humans" => humans = value.parse().map_err(|_| invalid("Invalid human count"))?,
            "--keys-dir" => key_dir = Some(PathBuf::from(value)),
            "--tls-dir" => tls_dir = Some(PathBuf::from(value)),
            "--audit-dir" => audit_dir = Some(PathBuf::from(value)),
            "--ready-file" => ready_file = Some(PathBuf::from(value)),
            "--run-seconds" => duration = value.parse().map_err(|_| invalid("Invalid duration"))?,
            "--bot-delay-ms" => bot_ms = value.parse().map_err(|_| invalid("Invalid bot delay"))?,
            _ => return Err(invalid("Unknown option")),
        }
    }
    if !(1..=4).contains(&humans) || bot_ms > 60000 || duration > 86400 {
        return Err(invalid("Option out of range"));
    }
    let config = tls::config(
        &tls_dir.ok_or_else(|| invalid("--tls-dir is required; use --init-tls DIR first"))?,
    )?;
    let audit = match audit_dir {
        Some(path) => Some(Arc::new(audit::Audit::new(&path)?)),
        None => None,
    };
    let keys = Arc::new(keys(
        &key_dir.ok_or_else(|| invalid("--keys-dir is required"))?,
    )?);
    let listener = TcpListener::bind(&bind)?;
    listener.set_nonblocking(true)?;
    let address = listener.local_addr()?;
    if let Some(path) = ready_file {
        fs::write(path, address.to_string())?;
    }
    println!("CARD READY {address} humans={humans} private-hands=required transport=TLS1.2+");
    let game = Arc::new(Mutex::new(Game::new(humans)));
    let stop = Arc::new(AtomicBool::new(false));
    let count = Arc::new(AtomicUsize::new(0));
    let mut workers = Vec::new();
    let began = Instant::now();
    let mut bot = Instant::now();
    loop {
        if duration != 0 && began.elapsed() >= Duration::from_secs(duration) {
            break;
        }
        match listener.accept() {
            Ok((stream, _)) => {
                if count.load(Ordering::Relaxed) < 16 {
                    count.fetch_add(1, Ordering::Relaxed);
                    let connection = Connection(count.clone());
                    let g = game.clone();
                    let k = keys.clone();
                    let stop = stop.clone();
                    let config = config.clone();
                    let audit = audit.clone();
                    workers.push(thread::spawn(move || {
                        let _connection = connection;
                        // No packet contents/credentials in logs, including rejection paths.
                        if let Err(e) = serve(stream, g, k, stop, config, audit) {
                            println!("CARD DISCONNECT {e}");
                        }
                    }));
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {}
            Err(e) => return Err(e),
        }
        if bot.elapsed() >= Duration::from_millis(bot_ms.max(1)) {
            game.lock().unwrap().bot_step(&mut OsRandom)?;
            bot = Instant::now();
        }
        workers.retain(|w| !w.is_finished());
        thread::sleep(Duration::from_millis(10));
    }
    stop.store(true, Ordering::Relaxed);
    for worker in workers {
        let _ = worker.join();
    }
    println!("CARD STOP");
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("card_authority: {e}");
        std::process::exit(1);
    }
}
