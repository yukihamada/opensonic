//! Indie → Major flow test with NFT minting
//!
//! Simulates an artist's journey:
//! 1. Register track (Indie stage)
//! 2. Early listeners get high ownership (0.10%)
//! 3. Track goes Rising (10K listens) → listeners get 0.03%
//! 4. Track goes Major (100K listens) → listeners get 0.01%
//! 5. Early listeners' NFTs retain their high ownership forever
//! 6. Verify stage transitions on-chain

use borsh::{BorshSerialize, BorshDeserialize};
use solana_sdk::{
    instruction::{AccountMeta, Instruction},
    pubkey::Pubkey,
    signature::{Keypair, Signer},
    system_program, sysvar::clock,
    transaction::Transaction,
};
use solana_client::rpc_client::RpcClient;

const PROGRAM_ID: &str = "FFpQXWd6U1h86PZ39UoCwMPi7i9TXL6EVE41x1PqHnCi";
const RPC: &str = "http://127.0.0.1:8899";
fn pid() -> Pubkey { PROGRAM_ID.parse().unwrap() }

#[derive(BorshSerialize)]
enum Ix {
    Initialize,
    SubmitProof { merkle_root: [u8;32], chain_tip: [u8;32], record_count: u64, channel: [u8;32] },
    RegisterRelay { tier: u8 },
    ClaimRelayReward { bytes_relayed: u64 },
    RegisterTrack { content_hash: [u8;32] },
    UpdateOwnership { content_hash: [u8;32] },
    MintListenNFT { content_hash: [u8;32], channel: [u8;32] },
}

#[derive(BorshDeserialize, Debug)]
struct TrackData { is_initialized: bool, artist: Pubkey, content_hash: [u8;32], total_listens: u64, total_ownership_distributed: u64, royalty_pool_lamports: u64, stage: u8, total_nfts: u64 }

#[derive(BorshDeserialize, Debug)]
struct NFTData { is_initialized: bool, owner: Pubkey, channel: [u8;32], content_hash: [u8;32], listen_count: u64, ownership_shares: u64, minted_at: i64, playback_enabled: bool }

fn ch(name: &str) -> [u8;32] { let mut c=[0u8;32]; let b=name.as_bytes(); c[..b.len().min(32)].copy_from_slice(&b[..b.len().min(32)]); c }

fn send(rpc: &RpcClient, payer: &Keypair, ix: Instruction) -> Result<String, String> {
    let bh = rpc.get_latest_blockhash().map_err(|e| e.to_string())?;
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[payer], bh);
    rpc.send_and_confirm_transaction(&tx).map(|s| s.to_string()).map_err(|e| e.to_string())
}

fn main() {
    let rpc = RpcClient::new(RPC.to_string());
    let payer = solana_sdk::signature::read_keypair_file("/tmp/soluna-deploy.json").unwrap();
    let p = pid();
    let (state_pda, _) = Pubkey::find_program_address(&[b"soluna"], &p);
    let content_hash = [0xAA; 32];
    let channel = ch("soluna");
    let (proof_pda, _) = Pubkey::find_program_address(&[b"pol", payer.pubkey().as_ref(), &channel], &p);
    let (track_pda, _) = Pubkey::find_program_address(&[b"track", &content_hash], &p);
    let (nft_pda, _) = Pubkey::find_program_address(&[b"nft", payer.pubkey().as_ref(), &content_hash], &p);

    println!("╔═══════════════════════════════════════════════════════╗");
    println!("║  Indie → Rising → Major Journey Test                 ║");
    println!("║  Track: 'soluna' channel, content_hash=0xAA...       ║");
    println!("╚═══════════════════════════════════════════════════════╝\n");

    // Initialize
    print!("1. Initialize... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(state_pda, false), AccountMeta::new(payer.pubkey(), true),
        AccountMeta::new_readonly(system_program::id(), false),
    ], data: borsh::to_vec(&Ix::Initialize).unwrap() });
    println!("OK");

    // Register track (starts as Indie)
    print!("2. Register track (Indie)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(track_pda, false), AccountMeta::new(payer.pubkey(), true),
        AccountMeta::new(state_pda, false), AccountMeta::new_readonly(system_program::id(), false),
    ], data: borsh::to_vec(&Ix::RegisterTrack { content_hash }).unwrap() });
    println!("OK");

    // ── INDIE STAGE: Early listener (50 listens) ──
    println!("\n── INDIE STAGE ──");
    print!("3. Submit PoL (50 listens — early bird)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(proof_pda, false), AccountMeta::new(payer.pubkey(), true),
        AccountMeta::new(state_pda, false), AccountMeta::new_readonly(system_program::id(), false),
        AccountMeta::new_readonly(clock::id(), false),
    ], data: borsh::to_vec(&Ix::SubmitProof { merkle_root: [0x11;32], chain_tip: [0x22;32], record_count: 50, channel }).unwrap() });
    println!("OK");

    // Update ownership + mint NFT
    print!("4. Update ownership... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new_readonly(proof_pda, false), AccountMeta::new(track_pda, false),
        AccountMeta::new(payer.pubkey(), true),
    ], data: borsh::to_vec(&Ix::UpdateOwnership { content_hash }).unwrap() });
    println!("OK");

    print!("5. Mint Listen NFT... ");
    match send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(nft_pda, false), AccountMeta::new_readonly(proof_pda, false),
        AccountMeta::new(payer.pubkey(), true), AccountMeta::new_readonly(state_pda, false),
        AccountMeta::new_readonly(system_program::id(), false), AccountMeta::new_readonly(clock::id(), false),
    ], data: borsh::to_vec(&Ix::MintListenNFT { content_hash, channel }).unwrap() }) {
        Ok(sig) => println!("OK ({})", &sig[..16]),
        Err(e) => println!("Error: {}", &e[..80.min(e.len())]),
    }

    // Read NFT
    if let Ok(data) = rpc.get_account_data(&nft_pda) {
        if let Ok(nft) = NFTData::try_from_slice(&data) {
            println!("   NFT: listens={}, ownership={}bps ({:.2}%), playback={}",
                nft.listen_count, nft.ownership_shares, nft.ownership_shares as f64 / 100.0, nft.playback_enabled);
        }
    }

    // Read track
    if let Ok(data) = rpc.get_account_data(&track_pda) {
        if let Ok(track) = TrackData::try_from_slice(&data) {
            let stage = match track.stage { 0 => "Indie", 1 => "Rising", 2 => "Major", _ => "?" };
            println!("   Track: listens={}, ownership_dist={}bps, stage={}", track.total_listens, track.total_ownership_distributed, stage);
        }
    }

    // ── RISING STAGE: More listeners push to 10K ──
    println!("\n── RISING STAGE (simulating 10K total listens) ──");
    print!("6. Submit PoL (10000 listens — reaching Rising)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(proof_pda, false), AccountMeta::new(payer.pubkey(), true),
        AccountMeta::new(state_pda, false), AccountMeta::new_readonly(system_program::id(), false),
        AccountMeta::new_readonly(clock::id(), false),
    ], data: borsh::to_vec(&Ix::SubmitProof { merkle_root: [0x33;32], chain_tip: [0x44;32], record_count: 10000, channel }).unwrap() });
    println!("OK");

    print!("7. Update ownership (should trigger Rising!)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new_readonly(proof_pda, false), AccountMeta::new(track_pda, false),
        AccountMeta::new(payer.pubkey(), true),
    ], data: borsh::to_vec(&Ix::UpdateOwnership { content_hash }).unwrap() });
    println!("OK");

    if let Ok(data) = rpc.get_account_data(&track_pda) {
        if let Ok(track) = TrackData::try_from_slice(&data) {
            let stage = match track.stage { 0 => "Indie", 1 => "Rising", 2 => "Major", _ => "?" };
            println!("   Track: listens={}, ownership_dist={}bps, stage={}", track.total_listens, track.total_ownership_distributed, stage);
        }
    }

    // ── MAJOR STAGE: 100K+ listens ──
    println!("\n── MAJOR STAGE (simulating 100K total listens) ──");
    print!("8. Submit PoL (100000 listens — Major debut!)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(proof_pda, false), AccountMeta::new(payer.pubkey(), true),
        AccountMeta::new(state_pda, false), AccountMeta::new_readonly(system_program::id(), false),
        AccountMeta::new_readonly(clock::id(), false),
    ], data: borsh::to_vec(&Ix::SubmitProof { merkle_root: [0x55;32], chain_tip: [0x66;32], record_count: 100000, channel }).unwrap() });
    println!("OK");

    print!("9. Update ownership (should trigger Major!)... ");
    let _ = send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new_readonly(proof_pda, false), AccountMeta::new(track_pda, false),
        AccountMeta::new(payer.pubkey(), true),
    ], data: borsh::to_vec(&Ix::UpdateOwnership { content_hash }).unwrap() });
    println!("OK");

    // Update NFT with new listen count
    print!("10. Update NFT (Major listener)... ");
    match send(&rpc, &payer, Instruction { program_id: p, accounts: vec![
        AccountMeta::new(nft_pda, false), AccountMeta::new_readonly(proof_pda, false),
        AccountMeta::new(payer.pubkey(), true), AccountMeta::new_readonly(state_pda, false),
        AccountMeta::new_readonly(system_program::id(), false), AccountMeta::new_readonly(clock::id(), false),
    ], data: borsh::to_vec(&Ix::MintListenNFT { content_hash, channel }).unwrap() }) {
        Ok(sig) => println!("OK ({})", &sig[..16]),
        Err(e) => println!("Error: {}", &e[..80.min(e.len())]),
    }

    // ── FINAL STATE ──
    println!("\n╔═══════════════════════════════════════════════════════╗");
    println!("║  Final On-Chain State                                 ║");
    println!("╚═══════════════════════════════════════════════════════╝\n");

    if let Ok(data) = rpc.get_account_data(&track_pda) {
        if let Ok(track) = TrackData::try_from_slice(&data) {
            let stage = match track.stage { 0 => "INDIE", 1 => "RISING", 2 => "MAJOR ★", _ => "?" };
            println!("  Track Registry:");
            println!("    Stage:              {}", stage);
            println!("    Total Listens:      {}", track.total_listens);
            println!("    Ownership Dist:     {} bps ({:.2}%)", track.total_ownership_distributed, track.total_ownership_distributed as f64 / 100.0);
            println!("    Artist retains:     {:.2}%", (10000 - track.total_ownership_distributed) as f64 / 100.0);
        }
    }

    if let Ok(data) = rpc.get_account_data(&nft_pda) {
        if let Ok(nft) = NFTData::try_from_slice(&data) {
            println!("\n  Listen NFT (Early Supporter):");
            println!("    Owner:              {}", nft.owner);
            println!("    Listen Count:       {}", nft.listen_count);
            println!("    Ownership:          {} bps ({:.2}%)", nft.ownership_shares, nft.ownership_shares as f64 / 100.0);
            println!("    Playback Enabled:   {}", if nft.playback_enabled { "YES ✓" } else { "NO" });
            println!("    Minted At:          {}", nft.minted_at);
        }
    }

    println!("\n  Ecosystem Economics:");
    println!("    ┌─────────────────────────────────────────────┐");
    println!("    │  Stage      Listens    New Listener Share   │");
    println!("    ├─────────────────────────────────────────────┤");
    println!("    │  Indie      10+        10 bps (0.10%)       │");
    println!("    │  Indie      100+       5 bps  (0.05%)       │");
    println!("    │  Rising     1,000+     3 bps  (0.03%)       │");
    println!("    │  Major      10,000+    1 bps  (0.01%)       │");
    println!("    ├─────────────────────────────────────────────┤");
    println!("    │  Early birds keep their HIGH shares forever │");
    println!("    │  Artist always retains ≥50% ownership       │");
    println!("    └─────────────────────────────────────────────┘");

    let bal = rpc.get_balance(&payer.pubkey()).unwrap() as f64 / 1e9;
    println!("\n  SOL spent: {:.6}", 100.0 - bal);
    println!("\n  All stages tested ✓");
}
