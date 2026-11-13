//! Authoritative four-seat Old Maid. No full-game serializer exists.
#![forbid(unsafe_code)]
use std::io;

pub const REQUEST_SIZE: usize = 40;
pub const RESPONSE_SIZE: usize = 64;
pub const NONE: u8 = 255;
pub const HELLO: u8 = 1;
pub const POLL: u8 = 2;
pub const DRAW: u8 = 3;
pub const RESTART: u8 = 4;

pub trait Random {
    fn below(&mut self, limit: usize) -> io::Result<usize>;
}
pub struct OsRandom;
impl Random for OsRandom {
    fn below(&mut self, limit: usize) -> io::Result<usize> {
        assert!(limit > 0 && limit <= 53);
        let limit = limit as u32;
        let ceiling = u32::MAX - u32::MAX % limit;
        loop {
            let mut bytes = [0u8; 4];
            getrandom::fill(&mut bytes).map_err(|e| io::Error::other(e.to_string()))?;
            let n = u32::from_le_bytes(bytes);
            if n < ceiling {
                return Ok((n % limit) as usize);
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub kind: u8,
    pub arg: u8,
    pub sequence: u64,
    pub revision: u64,
    pub token: [u8; 16],
}
impl Request {
    pub fn decode(bytes: &[u8]) -> Result<Self, &'static str> {
        if bytes.len() != REQUEST_SIZE || &bytes[..4] != b"ACAR" || bytes[4] != 1 || bytes[7] != 0 {
            return Err("invalid envelope");
        }
        let r = Self {
            kind: bytes[5],
            arg: bytes[6],
            sequence: u64::from_le_bytes(bytes[8..16].try_into().unwrap()),
            revision: u64::from_le_bytes(bytes[16..24].try_into().unwrap()),
            token: bytes[24..40].try_into().unwrap(),
        };
        if !(HELLO..=RESTART).contains(&r.kind) {
            return Err("unknown operation");
        }
        if r.kind == HELLO {
            if r.arg >= 4 || r.sequence != 0 || r.revision != 0 {
                return Err("invalid hello");
            }
        } else {
            if r.token != [0; 16] {
                return Err("credentials only in hello");
            }
            if r.kind != DRAW && r.arg != 0 {
                return Err("invalid argument");
            }
            if r.kind == POLL && (r.sequence != 0 || r.revision != 0) {
                return Err("invalid poll");
            }
        }
        Ok(r)
    }
}

#[derive(Clone)]
pub struct Game {
    hands: [Vec<u8>; 4], // IDs 0..51 = suit*13+rank; 52 = joker. Never sent collectively.
    pub revision: u64,
    pub round: u32,
    pub next_sequence: [u64; 4],
    pub humans: u8,
    pub connected: u8,
    pub turn: u8,
    pub victim: u8,
    pub loser: u8,
    discarded: u8,
}
impl Game {
    pub fn new(humans: u8) -> Self {
        assert!((1..=4).contains(&humans));
        Self {
            hands: std::array::from_fn(|_| Vec::new()),
            revision: 0,
            round: 0,
            next_sequence: [1; 4],
            humans: (1 << humans) - 1,
            connected: 0,
            turn: NONE,
            victim: NONE,
            loser: NONE,
            discarded: 0,
        }
    }
    pub fn ready(&self) -> bool {
        self.connected & self.humans == self.humans
    }
    pub fn phase(&self) -> u8 {
        // Keep the terminal result observable when another client exits after the match.
        if self.round != 0 && self.loser != NONE {
            2
        } else if !self.ready() || self.round == 0 {
            0
        } else {
            1
        }
    }
    pub fn claim(&mut self, seat: u8, rng: &mut impl Random) -> io::Result<bool> {
        if seat >= 4 || self.humans & (1 << seat) == 0 || self.connected & (1 << seat) != 0 {
            return Ok(false);
        }
        self.connected |= 1 << seat;
        self.revision += 1;
        if self.ready() && self.round == 0 {
            if let Err(error) = self.deal(rng) {
                self.connected &= !(1 << seat);
                self.revision -= 1;
                return Err(error); // Failed randomness must not leave an unowned seat claimed.
            }
        }
        Ok(true)
    }
    pub fn release(&mut self, seat: u8) {
        self.connected &= !(1 << seat);
        self.revision += 1; // Pause; never replace a disconnected human with a local CPU.
    }
    fn pairs(&mut self, seat: usize) {
        let mut kept = Vec::new();
        for &card in &self.hands[seat] {
            if let Some(index) = kept
                .iter()
                .position(|&c| card != 52 && c != 52 && c % 13 == card % 13)
            {
                kept.remove(index);
                self.discarded += 1;
            } else {
                kept.push(card);
            }
        }
        self.hands[seat] = kept;
    }
    fn next(&self, from: u8) -> u8 {
        (1..4)
            .map(|i| (from + i) % 4)
            .find(|&s| !self.hands[s as usize].is_empty())
            .unwrap_or(NONE)
    }
    fn check_end(&mut self) {
        let active: Vec<_> = (0..4).filter(|&s| !self.hands[s].is_empty()).collect();
        if active.len() == 1 {
            self.loser = active[0] as u8;
            self.turn = NONE;
            self.victim = NONE;
        }
    }
    pub fn deal(&mut self, rng: &mut impl Random) -> io::Result<()> {
        let mut deck: Vec<u8> = (0..53).collect();
        for i in (1..deck.len()).rev() {
            let j = rng.below(i + 1)?;
            deck.swap(i, j);
        }
        self.hands = std::array::from_fn(|_| Vec::new());
        self.discarded = 0;
        for (i, c) in deck.into_iter().enumerate() {
            self.hands[i % 4].push(c);
        }
        for s in 0..4 {
            self.pairs(s);
        }
        self.turn = (0..4).find(|&s| !self.hands[s].is_empty()).unwrap() as u8;
        self.victim = self.next(self.turn);
        self.loser = NONE;
        self.round += 1;
        self.revision += 1;
        self.check_end();
        Ok(())
    }
    // Only the server chooses the victim, moves the card, removes pairs and advances the turn.
    fn draw(&mut self, index: usize) {
        let turn = self.turn as usize;
        let victim = self.victim as usize;
        let card = self.hands[victim].remove(index);
        self.hands[turn].push(card);
        self.pairs(turn);
        self.revision += 1;
        self.check_end();
        if self.loser == NONE {
            self.turn = self.next(turn as u8);
            self.victim = self.next(self.turn);
        }
    }
    pub fn command(&mut self, seat: u8, r: &Request, rng: &mut impl Random) -> io::Result<u8> {
        if r.kind == POLL {
            return Ok(0);
        }
        if r.kind != DRAW && r.kind != RESTART {
            return Ok(6);
        }
        if r.sequence != self.next_sequence[seat as usize] {
            return Ok(4);
        }
        self.next_sequence[seat as usize] += 1; // Consume every well-sequenced attempt, including rejections.
        if r.revision != self.revision {
            return Ok(3);
        }
        if !self.ready() {
            return Ok(6);
        }
        if r.kind == RESTART {
            if self.phase() != 2 {
                return Ok(5);
            }
            self.deal(rng)?;
            return Ok(0);
        }
        if self.phase() != 1 || self.turn != seat {
            return Ok(1);
        }
        if r.arg as usize >= self.hands[self.victim as usize].len() {
            return Ok(2);
        }
        self.draw(r.arg as usize);
        Ok(0)
    }
    pub fn bot_step(&mut self, rng: &mut impl Random) -> io::Result<bool> {
        if self.phase() != 1 || self.humans & (1 << self.turn) != 0 {
            return Ok(false);
        }
        let i = rng.below(self.hands[self.victim as usize].len())?;
        self.draw(i);
        Ok(true)
    }
    pub fn snapshot(&self, seat: u8, error: u8) -> [u8; RESPONSE_SIZE] {
        assert!(seat < 4);
        let mut b = [0u8; RESPONSE_SIZE];
        b[..4].copy_from_slice(b"ACAR");
        b[4] = 1;
        b[5] = 128;
        b[6] = error;
        b[7] = seat;
        b[8..16].copy_from_slice(&self.next_sequence[seat as usize].to_le_bytes());
        b[16..24].copy_from_slice(&self.revision.to_le_bytes());
        b[24..28].copy_from_slice(&self.round.to_le_bytes());
        b[28] = self.turn;
        b[29] = self.victim;
        b[30] = self.loser;
        // These are the only private values that cross the boundary: this recipient's hand.
        let own = &self.hands[seat as usize];
        b[31] = own.len() as u8;
        for s in 0..4 {
            b[32 + s] = self.hands[s].len() as u8;
        }
        b[36] = self.discarded;
        b[37] = self.phase();
        b[38] = self.humans;
        b[40..40 + own.len()].copy_from_slice(own);
        b
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct TestRandom(u64);
    impl Random for TestRandom {
        fn below(&mut self, n: usize) -> io::Result<usize> {
            self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1);
            Ok(((self.0 >> 32) as usize) % n)
        }
    }
    fn running() -> (Game, TestRandom) {
        let mut rng = TestRandom(42);
        let mut g = Game::new(4);
        for s in 0..4 {
            assert!(g.claim(s, &mut rng).unwrap());
        }
        (g, rng)
    }
    fn draw(g: &Game, seat: u8) -> Request {
        Request {
            kind: DRAW,
            arg: 0,
            sequence: g.next_sequence[seat as usize],
            revision: g.revision,
            token: [0; 16],
        }
    }
    #[test]
    fn wire_strict_bounds() {
        for n in 0..REQUEST_SIZE {
            assert!(Request::decode(&vec![0; n]).is_err());
        }
        let mut b = [0u8; REQUEST_SIZE];
        b[..4].copy_from_slice(b"ACAR");
        b[4] = 1;
        b[5] = HELLO;
        assert!(Request::decode(&b).is_ok());
        b[7] = 1;
        assert!(Request::decode(&b).is_err());
        b[7] = 0;
        b[5] = 99;
        assert!(Request::decode(&b).is_err());
        b[5] = POLL;
        b[24] = 1;
        assert!(Request::decode(&b).is_err());
    }
    #[test]
    fn no_opponent_values_in_snapshot() {
        let (g, _) = running();
        for seat in 0..4 {
            let others: Vec<_> = (0..4).filter(|&s| s != seat).collect();
            let mut other = g.clone();
            // Change two private hands while holding public counts and recipient hand fixed.
            let a = others[0] as usize;
            let b = others[1] as usize;
            let first = other.hands[a][0];
            other.hands[a][0] = other.hands[b][0];
            other.hands[b][0] = first;
            assert_eq!(g.snapshot(seat, 0), other.snapshot(seat, 0));
            assert_eq!(
                &g.snapshot(seat, 0)[40..40 + g.hands[seat as usize].len()],
                g.hands[seat as usize]
            );
        }
    }
    #[test]
    fn rejects_forgery_stale_and_duplicate_without_mutating_cards() {
        let (mut g, mut rng) = running();
        let seat = g.turn;
        let initial = g.hands.clone();
        let mut r = draw(&g, seat);
        r.arg = 255;
        assert_eq!(g.command(seat, &r, &mut rng).unwrap(), 2);
        assert_eq!(initial, g.hands);
        let wrong = (seat + 1) % 4;
        assert_eq!(g.command(wrong, &draw(&g, wrong), &mut rng).unwrap(), 1);
        assert_eq!(initial, g.hands);
        let mut stale = draw(&g, seat);
        stale.revision -= 1;
        assert_eq!(g.command(seat, &stale, &mut rng).unwrap(), 3);
        assert_eq!(initial, g.hands);
        let r = draw(&g, seat);
        assert_eq!(g.command(seat, &r, &mut rng).unwrap(), 0);
        let after = g.hands.clone();
        let rev = g.revision;
        assert_eq!(g.command(seat, &r, &mut rng).unwrap(), 4);
        assert_eq!(after, g.hands);
        assert_eq!(rev, g.revision);
    }
    #[test]
    fn disconnect_pauses_and_reconnect_preserves_round_and_sequence() {
        let (mut g, mut rng) = running();
        let hands = g.hands.clone();
        let seq = g.next_sequence;
        g.release(2);
        assert_eq!(g.phase(), 0);
        assert!(!g.bot_step(&mut rng).unwrap());
        assert!(g.claim(2, &mut rng).unwrap());
        assert_eq!(hands, g.hands);
        assert_eq!(g.round, 1);
        assert_eq!(seq, g.next_sequence);
        assert!(!g.claim(2, &mut rng).unwrap());
    }
    #[test]
    fn complete_games_conserve_cards_and_retry_is_server_only() {
        for seed in 1..30 {
            let mut rng = TestRandom(seed);
            let mut g = Game::new(1);
            g.claim(0, &mut rng).unwrap();
            assert_eq!(
                g.command(
                    0,
                    &Request {
                        kind: RESTART,
                        ..draw(&g, 0)
                    },
                    &mut rng
                )
                .unwrap(),
                5
            );
            for _ in 0..10000 {
                let cards: Vec<_> = g.hands.iter().flatten().copied().collect();
                assert_eq!(cards.len() + 2 * g.discarded as usize, 53);
                assert_eq!(cards.iter().filter(|&&c| c == 52).count(), 1);
                let mut unique = cards.clone();
                unique.sort();
                unique.dedup();
                assert_eq!(unique.len(), cards.len());
                if g.phase() == 2 {
                    break;
                }
                if g.turn == 0 {
                    assert_eq!(g.command(0, &draw(&g, 0), &mut rng).unwrap(), 0);
                } else {
                    g.bot_step(&mut rng).unwrap();
                }
            }
            assert_eq!(g.phase(), 2);
            assert_eq!(g.discarded, 26);
            assert_eq!(g.hands[g.loser as usize], vec![52]);
            g.release(0);
            assert_eq!(g.phase(), 2); // A departing peer cannot hide the result.
            assert!(g.claim(0, &mut rng).unwrap());
            assert_eq!(g.phase(), 2);
            let r = Request {
                kind: RESTART,
                ..draw(&g, 0)
            };
            assert_eq!(g.command(0, &r, &mut rng).unwrap(), 0);
            assert_eq!(g.round, 2);
        }
    }
    #[test]
    fn failed_randomness_does_not_claim_a_seat_or_change_cards() {
        struct Failed;
        impl Random for Failed {
            fn below(&mut self, _: usize) -> io::Result<usize> {
                Err(io::Error::other("test RNG failure"))
            }
        }
        let mut game = Game::new(1);
        let before = game.snapshot(0, 0);
        assert!(game.claim(0, &mut Failed).is_err());
        assert_eq!(game.connected, 0);
        assert_eq!(game.snapshot(0, 0), before);
        assert!(game.claim(0, &mut TestRandom(1)).unwrap());
    }
}
