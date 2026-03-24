//! Integration test: Initialize → SubmitProof → RegisterRelay → ClaimReward → RegisterTrack

use borsh::BorshSerialize;
use solana_sdk::{
    instruction::{AccountMeta, Instruction},
    pubkey::Pubkey,
    signature::{Keypair, Signer},
    system_program,
    sysvar::clock,
    transaction::Transaction,
};
use solana_client::rpc_client::RpcClient;

const PROGRAM_ID: &str = "FFpQXWd6U1h86PZ39UoCwMPi7i9TXL6EVE41x1PqHnCi";
const RPC_URL: &str = "http://127.0.0.1:8899";

fn program_id() -> Pubkey {
    PROGRAM_ID.parse().unwrap()
}

/// Borsh-serialized instruction enum (must match on-chain program)
#[derive(BorshSerialize)]
enum SolunaInstruction {
    Initialize,
    SubmitProof {
        merkle_root: [u8; 32],
        chain_tip: [u8; 32],
        record_count: u64,
        channel: [u8; 32],
    },
    RegisterRelay {
        tier: u8,
    },
    ClaimRelayReward {
        bytes_relayed: u64,
    },
    RegisterTrack {
        content_hash: [u8; 32],
    },
    UpdateOwnership {
        content_hash: [u8; 32],
    },
}

fn main() {
    let rpc = RpcClient::new(RPC_URL.to_string());
    let pid = program_id();

    // Load deploy keypair (has SOL)
    let payer = solana_sdk::signature::read_keypair_file("/tmp/soluna-deploy.json")
        .expect("Failed to read keypair");

    println!("Payer: {}", payer.pubkey());
    println!("Balance: {} SOL", rpc.get_balance(&payer.pubkey()).unwrap() as f64 / 1e9);

    // 1. Initialize
    println!("\n=== 1. Initialize ===");
    let (state_pda, _) = Pubkey::find_program_address(&[b"soluna"], &pid);
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new(state_pda, false),
            AccountMeta::new(payer.pubkey(), true),
            AccountMeta::new_readonly(system_program::id(), false),
        ],
        data: borsh::to_vec(&SolunaInstruction::Initialize).unwrap(),
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // 2. Submit PoL Proof
    println!("\n=== 2. Submit Proof of Listen ===");
    let mut channel = [0u8; 32];
    channel[..4].copy_from_slice(b"jazz");
    let merkle_root = [0xAA; 32];
    let chain_tip = [0xBB; 32];

    let (proof_pda, _) = Pubkey::find_program_address(
        &[b"pol", payer.pubkey().as_ref(), &channel], &pid,
    );
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new(proof_pda, false),
            AccountMeta::new(payer.pubkey(), true),
            AccountMeta::new(state_pda, false),
            AccountMeta::new_readonly(system_program::id(), false),
            AccountMeta::new_readonly(clock::id(), false),
        ],
        data: borsh::to_vec(&SolunaInstruction::SubmitProof {
            merkle_root,
            chain_tip,
            record_count: 500,
            channel,
        }).unwrap(),
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // 3. Register Relay Node
    println!("\n=== 3. Register Relay Node ===");
    let (relay_pda, _) = Pubkey::find_program_address(
        &[b"relay", payer.pubkey().as_ref()], &pid,
    );
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new(relay_pda, false),
            AccountMeta::new(payer.pubkey(), true),
            AccountMeta::new(state_pda, false),
            AccountMeta::new_readonly(system_program::id(), false),
        ],
        data: borsh::to_vec(&SolunaInstruction::RegisterRelay { tier: 2 }).unwrap(), // Edge tier
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // 4. Claim Relay Reward
    println!("\n=== 4. Claim Relay Mining Reward ===");
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new(relay_pda, false),
            AccountMeta::new(payer.pubkey(), true),
            AccountMeta::new_readonly(state_pda, false),
            AccountMeta::new_readonly(clock::id(), false),
        ],
        data: borsh::to_vec(&SolunaInstruction::ClaimRelayReward {
            bytes_relayed: 170_000_000, // ~1 hour of 48kHz mono
        }).unwrap(),
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // 5. Register Track (Stream-to-Own)
    println!("\n=== 5. Register Track (Stream-to-Own) ===");
    let content_hash = [0xCC; 32]; // Simulated audio content hash
    let (track_pda, _) = Pubkey::find_program_address(
        &[b"track", &content_hash], &pid,
    );
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new(track_pda, false),
            AccountMeta::new(payer.pubkey(), true),
            AccountMeta::new(state_pda, false),
            AccountMeta::new_readonly(system_program::id(), false),
        ],
        data: borsh::to_vec(&SolunaInstruction::RegisterTrack { content_hash }).unwrap(),
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // 6. Update Ownership
    println!("\n=== 6. Update Ownership (Stream-to-Own) ===");
    let ix = Instruction {
        program_id: pid,
        accounts: vec![
            AccountMeta::new_readonly(proof_pda, false),
            AccountMeta::new(track_pda, false),
            AccountMeta::new(payer.pubkey(), true),
        ],
        data: borsh::to_vec(&SolunaInstruction::UpdateOwnership { content_hash }).unwrap(),
    };
    let bh = rpc.get_latest_blockhash().unwrap();
    let tx = Transaction::new_signed_with_payer(&[ix], Some(&payer.pubkey()), &[&payer], bh);
    match rpc.send_and_confirm_transaction(&tx) {
        Ok(sig) => println!("  OK: {sig}"),
        Err(e) => println!("  Error: {e}"),
    }

    // Summary
    let final_balance = rpc.get_balance(&payer.pubkey()).unwrap() as f64 / 1e9;
    println!("\n=== Summary ===");
    println!("Program ID: {pid}");
    println!("State PDA: {state_pda}");
    println!("PoL Proof PDA: {proof_pda}");
    println!("Relay PDA: {relay_pda}");
    println!("Track PDA: {track_pda}");
    println!("Final balance: {final_balance:.6} SOL");
    println!("\nAll 6 instructions tested!");
}
