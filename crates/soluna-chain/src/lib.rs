//! Solana client for Soluna on-chain interactions.
//!
//! Provides high-level functions for:
//! - Submitting PoL proofs
//! - Claiming relay mining rewards
//! - Registering tracks and updating ownership

use borsh::BorshSerialize;
use solana_client::rpc_client::RpcClient;
use solana_sdk::{
    instruction::{AccountMeta, Instruction},
    pubkey::Pubkey,
    signature::{Keypair, Signer},
    system_program,
    sysvar::clock,
    transaction::Transaction,
};

use soluna_core::pol::ProofSnapshot;

/// Program ID (placeholder — update after deploy)
pub fn program_id() -> Pubkey {
    "FFpQXWd6U1h86PZ39UoCwMPi7i9TXL6EVE41x1PqHnCi"
        .parse()
        .unwrap()
}

/// Solana RPC endpoint
const DEFAULT_RPC: &str = "https://api.mainnet-beta.solana.com";

/// Client for interacting with the Soluna on-chain program.
pub struct SolunaClient {
    rpc: RpcClient,
    program_id: Pubkey,
}

impl SolunaClient {
    pub fn new(rpc_url: &str) -> Self {
        Self {
            rpc: RpcClient::new(rpc_url.to_string()),
            program_id: program_id(),
        }
    }

    pub fn mainnet() -> Self {
        Self::new(DEFAULT_RPC)
    }

    pub fn devnet() -> Self {
        Self::new("https://api.devnet.solana.com")
    }

    /// Submit a Proof of Listen to Solana.
    pub fn submit_pol(
        &self,
        payer: &Keypair,
        snapshot: &ProofSnapshot,
    ) -> Result<String, Box<dyn std::error::Error>> {
        let mut channel_bytes = [0u8; 32];
        let ch = snapshot.channel.as_bytes();
        let len = ch.len().min(32);
        channel_bytes[..len].copy_from_slice(&ch[..len]);

        // Derive PDA for this listener+channel
        let (proof_pda, _) = Pubkey::find_program_address(
            &[b"pol", payer.pubkey().as_ref(), &channel_bytes],
            &self.program_id,
        );

        let (state_pda, _) = Pubkey::find_program_address(
            &[b"soluna"],
            &self.program_id,
        );

        #[derive(BorshSerialize)]
        enum Ix {
            _Init,
            SubmitProof {
                merkle_root: [u8; 32],
                chain_tip: [u8; 32],
                record_count: u64,
                channel: [u8; 32],
            },
        }

        let ix_data = Ix::SubmitProof {
            merkle_root: snapshot.merkle_root,
            chain_tip: snapshot.chain_tip,
            record_count: snapshot.record_count,
            channel: channel_bytes,
        };

        let instruction = Instruction {
            program_id: self.program_id,
            accounts: vec![
                AccountMeta::new(proof_pda, false),
                AccountMeta::new(payer.pubkey(), true),
                AccountMeta::new(state_pda, false),
                AccountMeta::new_readonly(system_program::id(), false),
                AccountMeta::new_readonly(clock::id(), false),
            ],
            data: borsh::to_vec(&ix_data)?,
        };

        let recent_blockhash = self.rpc.get_latest_blockhash()?;
        let tx = Transaction::new_signed_with_payer(
            &[instruction],
            Some(&payer.pubkey()),
            &[payer],
            recent_blockhash,
        );

        let sig = self.rpc.send_and_confirm_transaction(&tx)?;
        Ok(sig.to_string())
    }

    /// Register a relay node for mining rewards.
    pub fn register_relay(
        &self,
        payer: &Keypair,
        tier: u8,
    ) -> Result<String, Box<dyn std::error::Error>> {
        let (relay_pda, _) = Pubkey::find_program_address(
            &[b"relay", payer.pubkey().as_ref()],
            &self.program_id,
        );

        let (state_pda, _) = Pubkey::find_program_address(
            &[b"soluna"],
            &self.program_id,
        );

        #[derive(BorshSerialize)]
        enum Ix {
            _Init,
            _SubmitProof,
            RegisterRelay { tier: u8 },
        }

        let ix_data = Ix::RegisterRelay { tier };

        let instruction = Instruction {
            program_id: self.program_id,
            accounts: vec![
                AccountMeta::new(relay_pda, false),
                AccountMeta::new(payer.pubkey(), true),
                AccountMeta::new(state_pda, false),
                AccountMeta::new_readonly(system_program::id(), false),
            ],
            data: borsh::to_vec(&ix_data)?,
        };

        let recent_blockhash = self.rpc.get_latest_blockhash()?;
        let tx = Transaction::new_signed_with_payer(
            &[instruction],
            Some(&payer.pubkey()),
            &[payer],
            recent_blockhash,
        );

        let sig = self.rpc.send_and_confirm_transaction(&tx)?;
        Ok(sig.to_string())
    }

    /// Claim relay mining rewards.
    pub fn claim_relay_reward(
        &self,
        payer: &Keypair,
        bytes_relayed: u64,
    ) -> Result<String, Box<dyn std::error::Error>> {
        let (relay_pda, _) = Pubkey::find_program_address(
            &[b"relay", payer.pubkey().as_ref()],
            &self.program_id,
        );

        let (state_pda, _) = Pubkey::find_program_address(
            &[b"soluna"],
            &self.program_id,
        );

        #[derive(BorshSerialize)]
        enum Ix {
            _Init,
            _SubmitProof,
            _RegisterRelay,
            ClaimRelayReward { bytes_relayed: u64 },
        }

        let ix_data = Ix::ClaimRelayReward { bytes_relayed };

        let instruction = Instruction {
            program_id: self.program_id,
            accounts: vec![
                AccountMeta::new(relay_pda, false),
                AccountMeta::new(payer.pubkey(), true),
                AccountMeta::new_readonly(state_pda, false),
                AccountMeta::new_readonly(clock::id(), false),
            ],
            data: borsh::to_vec(&ix_data)?,
        };

        let recent_blockhash = self.rpc.get_latest_blockhash()?;
        let tx = Transaction::new_signed_with_payer(
            &[instruction],
            Some(&payer.pubkey()),
            &[payer],
            recent_blockhash,
        );

        let sig = self.rpc.send_and_confirm_transaction(&tx)?;
        Ok(sig.to_string())
    }

    /// Register a track for Stream-to-Own.
    pub fn register_track(
        &self,
        artist: &Keypair,
        content_hash: [u8; 32],
    ) -> Result<String, Box<dyn std::error::Error>> {
        let (track_pda, _) = Pubkey::find_program_address(
            &[b"track", &content_hash],
            &self.program_id,
        );

        let (state_pda, _) = Pubkey::find_program_address(
            &[b"soluna"],
            &self.program_id,
        );

        #[derive(BorshSerialize)]
        enum Ix {
            _Init,
            _SubmitProof,
            _RegisterRelay,
            _ClaimRelayReward,
            RegisterTrack { content_hash: [u8; 32] },
        }

        let ix_data = Ix::RegisterTrack { content_hash };

        let instruction = Instruction {
            program_id: self.program_id,
            accounts: vec![
                AccountMeta::new(track_pda, false),
                AccountMeta::new(artist.pubkey(), true),
                AccountMeta::new(state_pda, false),
                AccountMeta::new_readonly(system_program::id(), false),
            ],
            data: borsh::to_vec(&ix_data)?,
        };

        let recent_blockhash = self.rpc.get_latest_blockhash()?;
        let tx = Transaction::new_signed_with_payer(
            &[instruction],
            Some(&artist.pubkey()),
            &[artist],
            recent_blockhash,
        );

        let sig = self.rpc.send_and_confirm_transaction(&tx)?;
        Ok(sig.to_string())
    }
}
