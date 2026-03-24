//! Proof of Listen (PoL) — 聴いた証明プロトコル
//!
//! OSTP パケットの受信を暗号学的に証明する。
//! リスナーが「確かにこの音声ストリームを受信した」ことを、
//! 第三者（Solana スマートコントラクト等）が検証可能。
//!
//! # 仕組み
//!
//! 1. 各受信パケットから `(seq, timestamp, ssrc, crc32)` を抽出
//! 2. SHA-256 でハッシュチェーンを構築（前のハッシュ + 現パケット情報）
//! 3. 定期的に Merkle Root を生成
//! 4. Merkle Root を Solana に提出 → 再生証明
//!
//! # 設計原則
//! - オーディオデータ自体はハッシュに含まない（プライバシー + 帯域節約）
//! - メタデータ（seq, ts, ssrc）だけで十分な証明力
//! - アーティスト側も同じハッシュチェーンを構築 → 照合で検証

use std::collections::VecDeque;

/// SHA-256 ハッシュ (32 bytes)
pub type Hash256 = [u8; 32];

/// パケット受信の証拠レコード
#[derive(Debug, Clone, Copy)]
pub struct ListenRecord {
    /// RTP sequence number
    pub seq: u16,
    /// RTP timestamp
    pub timestamp: u32,
    /// Sender SSRC
    pub ssrc: u32,
    /// Payload CRC-32 (from OSTP trailer, or computed)
    pub payload_crc: u32,
    /// Unix timestamp (ms) when received
    pub received_at: u64,
}

impl ListenRecord {
    /// Serialize to bytes for hashing (fixed 22 bytes).
    pub fn to_bytes(&self) -> [u8; 22] {
        let mut buf = [0u8; 22];
        buf[0..2].copy_from_slice(&self.seq.to_be_bytes());
        buf[2..6].copy_from_slice(&self.timestamp.to_be_bytes());
        buf[6..10].copy_from_slice(&self.ssrc.to_be_bytes());
        buf[10..14].copy_from_slice(&self.payload_crc.to_be_bytes());
        buf[14..22].copy_from_slice(&self.received_at.to_be_bytes());
        buf
    }
}

/// PoL ハッシュチェーン — パケット受信ごとにチェーンを伸ばす。
///
/// ```
/// let mut chain = HashChain::new();
/// chain.append(ListenRecord { seq: 1, timestamp: 240, ssrc: 0x1234, payload_crc: 0xDEAD, received_at: 1710000000000 });
/// chain.append(ListenRecord { seq: 2, timestamp: 480, ssrc: 0x1234, payload_crc: 0xBEEF, received_at: 1710000000005 });
/// let root = chain.merkle_root();
/// assert_eq!(root.len(), 32);
/// ```
pub struct HashChain {
    /// Current chain tip hash
    tip: Hash256,
    /// Accumulated leaf hashes for Merkle tree construction
    leaves: VecDeque<Hash256>,
    /// Total records appended
    count: u64,
    /// Channel name (included in genesis hash)
    channel: String,
}

impl HashChain {
    /// Create a new chain for a channel.
    pub fn new() -> Self {
        Self::with_channel("")
    }

    /// Create a new chain for a specific channel.
    pub fn with_channel(channel: &str) -> Self {
        // Genesis hash includes channel name
        let genesis = sha256(&[b"soluna:pol:v1:", channel.as_bytes()].concat());
        Self {
            tip: genesis,
            leaves: VecDeque::new(),
            count: 0,
            channel: channel.to_string(),
        }
    }

    /// Append a listen record to the chain.
    ///
    /// Returns the new chain tip hash.
    pub fn append(&mut self, record: ListenRecord) -> Hash256 {
        // new_tip = SHA-256(prev_tip || record_bytes)
        let record_bytes = record.to_bytes();
        let mut input = Vec::with_capacity(32 + record_bytes.len());
        input.extend_from_slice(&self.tip);
        input.extend_from_slice(&record_bytes);
        let new_tip = sha256(&input);

        self.tip = new_tip;
        self.leaves.push_back(new_tip);
        self.count += 1;

        new_tip
    }

    /// Get the current chain tip hash.
    pub fn tip(&self) -> Hash256 {
        self.tip
    }

    /// Number of records in the chain.
    pub fn count(&self) -> u64 {
        self.count
    }

    /// Compute the Merkle root of all accumulated leaves.
    ///
    /// This is what gets submitted to Solana for verification.
    pub fn merkle_root(&self) -> Hash256 {
        if self.leaves.is_empty() {
            return self.tip; // Genesis
        }
        merkle_root_from_leaves(&self.leaves.iter().copied().collect::<Vec<_>>())
    }

    /// Drain leaves and return a proof snapshot.
    ///
    /// Call this periodically (e.g., every hour) to submit to Solana.
    /// Returns (merkle_root, count, channel) for on-chain submission.
    pub fn snapshot(&mut self) -> ProofSnapshot {
        let root = self.merkle_root();
        let count = self.count;
        let channel = self.channel.clone();
        self.leaves.clear();
        // Don't reset count or tip — chain continues
        ProofSnapshot {
            merkle_root: root,
            chain_tip: self.tip,
            record_count: count,
            channel,
        }
    }
}

/// A snapshot of the PoL chain for on-chain submission.
#[derive(Debug, Clone)]
pub struct ProofSnapshot {
    /// Merkle root of all listen records in this period
    pub merkle_root: Hash256,
    /// Current chain tip (for continuity verification)
    pub chain_tip: Hash256,
    /// Total records since chain creation
    pub record_count: u64,
    /// Channel name
    pub channel: String,
}

impl ProofSnapshot {
    /// Serialize for Solana instruction data (fixed 80 bytes).
    pub fn to_instruction_data(&self) -> Vec<u8> {
        let mut data = Vec::with_capacity(80);
        data.extend_from_slice(&self.merkle_root);       // 32 bytes
        data.extend_from_slice(&self.chain_tip);          // 32 bytes
        data.extend_from_slice(&self.record_count.to_le_bytes()); // 8 bytes
        // Channel name: 8 bytes (truncated/padded)
        let mut ch = [0u8; 8];
        let bytes = self.channel.as_bytes();
        let len = bytes.len().min(8);
        ch[..len].copy_from_slice(&bytes[..len]);
        data.extend_from_slice(&ch);                      // 8 bytes
        data
    }
}

// ── Merkle Tree ──

/// Compute Merkle root from leaf hashes.
fn merkle_root_from_leaves(leaves: &[Hash256]) -> Hash256 {
    if leaves.is_empty() {
        return [0u8; 32];
    }
    if leaves.len() == 1 {
        return leaves[0];
    }

    let mut level: Vec<Hash256> = leaves.to_vec();

    while level.len() > 1 {
        let mut next_level = Vec::with_capacity((level.len() + 1) / 2);
        for pair in level.chunks(2) {
            if pair.len() == 2 {
                let mut combined = [0u8; 64];
                combined[..32].copy_from_slice(&pair[0]);
                combined[32..].copy_from_slice(&pair[1]);
                next_level.push(sha256(&combined));
            } else {
                // Odd leaf: promote to next level
                next_level.push(pair[0]);
            }
        }
        level = next_level;
    }

    level[0]
}

/// Generate a Merkle proof for a specific leaf index.
pub fn merkle_proof(leaves: &[Hash256], index: usize) -> Vec<(Hash256, bool)> {
    if leaves.is_empty() || index >= leaves.len() {
        return vec![];
    }

    let mut proof = Vec::new();
    let mut level: Vec<Hash256> = leaves.to_vec();
    let mut idx = index;

    while level.len() > 1 {
        let sibling_idx = if idx % 2 == 0 { idx + 1 } else { idx - 1 };
        if sibling_idx < level.len() {
            // true = sibling is on the right
            proof.push((level[sibling_idx], idx % 2 == 0));
        }

        // Build next level
        let mut next_level = Vec::with_capacity((level.len() + 1) / 2);
        for pair in level.chunks(2) {
            if pair.len() == 2 {
                let mut combined = [0u8; 64];
                combined[..32].copy_from_slice(&pair[0]);
                combined[32..].copy_from_slice(&pair[1]);
                next_level.push(sha256(&combined));
            } else {
                next_level.push(pair[0]);
            }
        }
        level = next_level;
        idx /= 2;
    }

    proof
}

/// Verify a Merkle proof.
pub fn verify_merkle_proof(leaf: Hash256, proof: &[(Hash256, bool)], root: Hash256) -> bool {
    let mut current = leaf;
    for &(ref sibling, is_right) in proof {
        let mut combined = [0u8; 64];
        if is_right {
            combined[..32].copy_from_slice(&current);
            combined[32..].copy_from_slice(sibling);
        } else {
            combined[..32].copy_from_slice(sibling);
            combined[32..].copy_from_slice(&current);
        }
        current = sha256(&combined);
    }
    current == root
}

// ── SHA-256 (pure Rust, no dependencies) ──

fn sha256(data: &[u8]) -> Hash256 {
    // SHA-256 constants
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];

    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ];

    // Padding
    let bit_len = (data.len() as u64) * 8;
    let mut padded = data.to_vec();
    padded.push(0x80);
    while (padded.len() % 64) != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    // Process blocks
    for block in padded.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                block[i * 4], block[i * 4 + 1],
                block[i * 4 + 2], block[i * 4 + 3],
            ]);
        }
        for i in 16..64 {
            let s0 = w[i-15].rotate_right(7) ^ w[i-15].rotate_right(18) ^ (w[i-15] >> 3);
            let s1 = w[i-2].rotate_right(17) ^ w[i-2].rotate_right(19) ^ (w[i-2] >> 10);
            w[i] = w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;

        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let temp1 = hh.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = s0.wrapping_add(maj);

            hh = g; g = f; f = e;
            e = d.wrapping_add(temp1);
            d = c; c = b; b = a;
            a = temp1.wrapping_add(temp2);
        }

        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }

    let mut result = [0u8; 32];
    for (i, &val) in h.iter().enumerate() {
        result[i * 4..i * 4 + 4].copy_from_slice(&val.to_be_bytes());
    }
    result
}

/// Convert hash to hex string.
pub fn hash_to_hex(hash: &Hash256) -> String {
    hash.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_sha256_empty() {
        let hash = sha256(b"");
        assert_eq!(
            hash_to_hex(&hash),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    #[test]
    fn test_sha256_abc() {
        let hash = sha256(b"abc");
        assert_eq!(
            hash_to_hex(&hash),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn test_hash_chain_deterministic() {
        let mut chain1 = HashChain::with_channel("test");
        let mut chain2 = HashChain::with_channel("test");

        let record = ListenRecord {
            seq: 1,
            timestamp: 240,
            ssrc: 0x12345678,
            payload_crc: 0xDEADBEEF,
            received_at: 1710000000000,
        };

        chain1.append(record);
        chain2.append(record);

        assert_eq!(chain1.tip(), chain2.tip());
        assert_eq!(chain1.merkle_root(), chain2.merkle_root());
    }

    #[test]
    fn test_hash_chain_different_channels() {
        let mut chain1 = HashChain::with_channel("jazz");
        let mut chain2 = HashChain::with_channel("chill");

        let record = ListenRecord {
            seq: 1,
            timestamp: 240,
            ssrc: 0x12345678,
            payload_crc: 0xDEADBEEF,
            received_at: 1710000000000,
        };

        chain1.append(record);
        chain2.append(record);

        // Different channels → different hashes
        assert_ne!(chain1.tip(), chain2.tip());
    }

    #[test]
    fn test_hash_chain_order_matters() {
        let r1 = ListenRecord { seq: 1, timestamp: 240, ssrc: 0x1234, payload_crc: 0xAAAA, received_at: 1000 };
        let r2 = ListenRecord { seq: 2, timestamp: 480, ssrc: 0x1234, payload_crc: 0xBBBB, received_at: 1005 };

        let mut chain_a = HashChain::with_channel("test");
        chain_a.append(r1);
        chain_a.append(r2);

        let mut chain_b = HashChain::with_channel("test");
        chain_b.append(r2);
        chain_b.append(r1);

        // Different order → different result (proves sequence)
        assert_ne!(chain_a.tip(), chain_b.tip());
    }

    #[test]
    fn test_merkle_root_single_leaf() {
        let mut chain = HashChain::with_channel("test");
        chain.append(ListenRecord { seq: 1, timestamp: 240, ssrc: 0x1234, payload_crc: 0xAAAA, received_at: 1000 });
        let root = chain.merkle_root();
        assert_eq!(root, chain.tip()); // Single leaf = tip
    }

    #[test]
    fn test_merkle_proof_verify() {
        let leaves: Vec<Hash256> = (0..8u8).map(|i| sha256(&[i])).collect();
        let root = merkle_root_from_leaves(&leaves);

        for i in 0..leaves.len() {
            let proof = merkle_proof(&leaves, i);
            assert!(verify_merkle_proof(leaves[i], &proof, root),
                "Proof failed for leaf {i}");
        }
    }

    #[test]
    fn test_merkle_proof_tamper_detection() {
        let leaves: Vec<Hash256> = (0..4u8).map(|i| sha256(&[i])).collect();
        let root = merkle_root_from_leaves(&leaves);
        let proof = merkle_proof(&leaves, 0);

        // Tampered leaf should fail
        let fake_leaf = sha256(b"fake");
        assert!(!verify_merkle_proof(fake_leaf, &proof, root));
    }

    #[test]
    fn test_snapshot() {
        let mut chain = HashChain::with_channel("jazz");
        for i in 0..100u16 {
            chain.append(ListenRecord {
                seq: i,
                timestamp: i as u32 * 240,
                ssrc: 0xABCD,
                payload_crc: 0x1234,
                received_at: 1710000000000 + i as u64 * 5,
            });
        }

        let snap = chain.snapshot();
        assert_eq!(snap.record_count, 100);
        assert_eq!(snap.channel, "jazz");
        assert_eq!(snap.to_instruction_data().len(), 80);

        // Chain continues after snapshot
        chain.append(ListenRecord { seq: 100, timestamp: 24000, ssrc: 0xABCD, payload_crc: 0x5678, received_at: 1710000000500 });
        assert_eq!(chain.count(), 101);
    }
}
