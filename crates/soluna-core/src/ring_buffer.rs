/// Lock-free SPSC ring buffer for audio samples (f32).
///
/// Single producer (network thread) writes interleaved frames.
/// Single consumer (audio callback) reads interleaved frames.
/// Uses atomic indices for thread safety.
use core::sync::atomic::{AtomicUsize, Ordering};

pub struct RingBuffer {
    buf: Vec<f32>,
    /// Capacity in samples (not frames).
    cap: usize,
    /// Write position (monotonically increasing sample index).
    write_pos: AtomicUsize,
    /// Read position (monotonically increasing sample index).
    read_pos: AtomicUsize,
}

impl RingBuffer {
    /// Create a new ring buffer with capacity for `frames` frames of `channels` channels.
    pub fn new(frames: usize, channels: usize) -> Self {
        let cap = frames * channels;
        Self {
            buf: vec![0.0; cap],
            cap,
            write_pos: AtomicUsize::new(0),
            read_pos: AtomicUsize::new(0),
        }
    }

    /// Number of samples available for reading.
    pub fn available(&self) -> usize {
        let w = self.write_pos.load(Ordering::Acquire);
        let r = self.read_pos.load(Ordering::Acquire);
        w.wrapping_sub(r)
    }

    /// Write interleaved samples. Overwrites oldest data if full.
    pub fn write(&self, data: &[f32]) {
        let w = self.write_pos.load(Ordering::Relaxed);
        for (i, &s) in data.iter().enumerate() {
            let idx = (w + i) % self.cap;
            // Safety: single producer, no concurrent writes
            unsafe {
                let ptr = self.buf.as_ptr() as *mut f32;
                *ptr.add(idx) = s;
            }
        }
        self.write_pos.store(w.wrapping_add(data.len()), Ordering::Release);
    }

    /// Read up to `count` samples into `out`. Returns number of samples read.
    pub fn read(&self, out: &mut [f32]) -> usize {
        let avail = self.available();
        let n = out.len().min(avail);
        let r = self.read_pos.load(Ordering::Relaxed);
        for i in 0..n {
            let idx = (r + i) % self.cap;
            out[i] = self.buf[idx];
        }
        self.read_pos.store(r.wrapping_add(n), Ordering::Release);
        n
    }

    /// Discard all buffered data.
    pub fn clear(&self) {
        let w = self.write_pos.load(Ordering::Acquire);
        self.read_pos.store(w, Ordering::Release);
    }

    /// Capacity in samples.
    pub fn capacity(&self) -> usize {
        self.cap
    }
}

// RingBuffer uses interior mutability via atomics + unsafe single-producer write.
// Safe for SPSC (one writer thread, one reader thread).
unsafe impl Send for RingBuffer {}
unsafe impl Sync for RingBuffer {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_write_read() {
        let rb = RingBuffer::new(1024, 1);
        let data = [1.0, 2.0, 3.0, 4.0];
        rb.write(&data);
        assert_eq!(rb.available(), 4);

        let mut out = [0.0f32; 4];
        let n = rb.read(&mut out);
        assert_eq!(n, 4);
        assert_eq!(out, data);
        assert_eq!(rb.available(), 0);
    }

    #[test]
    fn test_partial_read() {
        let rb = RingBuffer::new(1024, 1);
        rb.write(&[1.0, 2.0, 3.0]);
        let mut out = [0.0f32; 2];
        let n = rb.read(&mut out);
        assert_eq!(n, 2);
        assert_eq!(out, [1.0, 2.0]);
        assert_eq!(rb.available(), 1);
    }
}
