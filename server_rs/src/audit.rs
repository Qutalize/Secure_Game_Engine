//! Explicit experiment recorder. Server-side files contain private data; never distribute them.
use std::{
    fs::{self, File},
    io::{self, Write},
    path::Path,
    sync::Mutex,
};
pub struct Audit {
    files: Mutex<[File; 4]>,
}
impl Audit {
    pub fn new(path: &Path) -> io::Result<Self> {
        fs::create_dir(path)?;
        let mut files = Vec::new();
        for seat in 0..4 {
            files.push(File::create(path.join(format!("seat{seat}.wire")))?);
        }
        let files: [File; 4] = files
            .try_into()
            .map_err(|_| io::Error::other("Audit file count"))?;
        Ok(Self {
            files: Mutex::new(files),
        })
    }
    pub fn record(&self, seat: u8, reply: &[u8]) -> io::Result<()> {
        let mut files = self.files.lock().unwrap();
        let file = &mut files[seat as usize];
        file.write_all(reply)?;
        file.flush()
    }
}
