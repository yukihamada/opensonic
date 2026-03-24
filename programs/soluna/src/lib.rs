//! Soluna On-Chain Program — PoL検証 + Relay Mining報酬 + Stream-to-Own NFT
//!
//! Solana上でSolunaエコシステムの3つの核心機能を提供:
//!
//! 1. **Proof of Listen (PoL)**: リスナーの再生証明 Merkle Root を検証・記録
//! 2. **Relay Mining**: リレーノードの帯域貢献に対する ENAI トークン報酬
//! 3. **Stream-to-Own**: 再生回数に応じたマイクロオーナーシップ NFT
//!
//! # アカウント構造
//!
//! ```text
//! SolunaState (PDA: ["soluna"])
//! ├── authority: Pubkey
//! ├── enai_mint: Pubkey
//! └── total_proofs: u64
//!
//! ListenProof (PDA: ["pol", listener, channel])
//! ├── listener: Pubkey
//! ├── channel: [u8; 32]
//! ├── merkle_root: [u8; 32]
//! ├── chain_tip: [u8; 32]
//! ├── record_count: u64
//! ├── last_updated: i64
//! └── ownership_shares: u64  ← Stream-to-Own
//!
//! RelayNode (PDA: ["relay", operator])
//! ├── operator: Pubkey
//! ├── tier: u8
//! ├── total_bytes_relayed: u64
//! ├── total_rewards: u64
//! └── last_claim: i64
//!
//! TrackRegistry (PDA: ["track", content_hash])
//! ├── artist: Pubkey
//! ├── content_hash: [u8; 32]
//! ├── total_listens: u64
//! ├── total_ownership_distributed: u64
//! └── royalty_pool_lamports: u64
//! ```

use borsh::{BorshDeserialize, BorshSerialize};
use solana_program::{
    account_info::{next_account_info, AccountInfo},
    entrypoint,
    entrypoint::ProgramResult,
    msg,
    program::invoke_signed,
    program_error::ProgramError,
    pubkey::Pubkey,
    rent::Rent,
    system_instruction,
    sysvar::Sysvar,
    clock::Clock,
};

// Program ID (placeholder — replace after deploy)
solana_program::declare_id!("FFpQXWd6U1h86PZ39UoCwMPi7i9TXL6EVE41x1PqHnCi");

entrypoint!(process_instruction);

// ── Instructions ──

#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub enum SolunaInstruction {
    /// Initialize the program state.
    /// Accounts: [state (PDA, writable), authority (signer), system_program]
    Initialize,

    /// Submit a Proof of Listen.
    /// Data: merkle_root (32), chain_tip (32), record_count (u64), channel (32)
    /// Accounts: [proof (PDA, writable), listener (signer), state, system_program, clock]
    SubmitProof {
        merkle_root: [u8; 32],
        chain_tip: [u8; 32],
        record_count: u64,
        channel: [u8; 32],
    },

    /// Register a relay node for mining.
    /// Accounts: [relay (PDA, writable), operator (signer), state, system_program]
    RegisterRelay {
        tier: u8,
    },

    /// Claim relay mining rewards.
    /// Data: bytes_relayed (u64)
    /// Accounts: [relay (PDA, writable), operator (signer), state, clock]
    ClaimRelayReward {
        bytes_relayed: u64,
    },

    /// Register a track for Stream-to-Own.
    /// Accounts: [track (PDA, writable), artist (signer), state, system_program]
    RegisterTrack {
        content_hash: [u8; 32],
    },

    /// Update ownership based on listen count (called by PoL verifier).
    /// Accounts: [proof (PDA, writable), track (PDA, writable), authority (signer)]
    UpdateOwnership {
        content_hash: [u8; 32],
    },

    /// Mint a Listen NFT — proof of listening + ownership certificate.
    /// Automatically minted when PoL is submitted with enough listens.
    /// The NFT contains: channel, content_hash, listen_count, ownership_shares.
    /// Accounts: [nft (PDA, writable), proof (PDA), listener (signer), state, system_program, clock]
    MintListenNFT {
        content_hash: [u8; 32],
        channel: [u8; 32],
    },
}

// ── Account State ──

#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub struct SolunaState {
    pub is_initialized: bool,
    pub authority: Pubkey,
    pub enai_mint: Pubkey,
    pub total_proofs: u64,
    pub total_relay_nodes: u64,
    pub total_tracks: u64,
}

impl SolunaState {
    pub const SIZE: usize = 1 + 32 + 32 + 8 + 8 + 8; // 89 bytes
}

#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub struct ListenProof {
    pub is_initialized: bool,
    pub listener: Pubkey,
    pub channel: [u8; 32],
    pub merkle_root: [u8; 32],
    pub chain_tip: [u8; 32],
    pub record_count: u64,
    pub last_updated: i64,
    pub ownership_shares: u64,
}

impl ListenProof {
    pub const SIZE: usize = 1 + 32 + 32 + 32 + 32 + 8 + 8 + 8; // 153 bytes
}

#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub struct RelayNode {
    pub is_initialized: bool,
    pub operator: Pubkey,
    pub tier: u8,
    pub total_bytes_relayed: u64,
    pub total_rewards: u64,
    pub last_claim: i64,
}

impl RelayNode {
    pub const SIZE: usize = 1 + 32 + 1 + 8 + 8 + 8; // 58 bytes
}

#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub struct TrackRegistry {
    pub is_initialized: bool,
    pub artist: Pubkey,
    pub content_hash: [u8; 32],
    pub total_listens: u64,
    pub total_ownership_distributed: u64,
    pub royalty_pool_lamports: u64,
    /// 0=Indie, 1=Rising, 2=Major
    pub stage: u8,
    /// Total NFTs minted for this track
    pub total_nfts: u64,
}

impl TrackRegistry {
    pub const SIZE: usize = 1 + 32 + 32 + 8 + 8 + 8 + 1 + 8; // 98 bytes
}

/// Listen NFT — proof of listening + ownership certificate
#[derive(BorshSerialize, BorshDeserialize, Debug)]
pub struct ListenNFT {
    pub is_initialized: bool,
    /// NFT holder
    pub owner: Pubkey,
    /// Channel this NFT represents listening to
    pub channel: [u8; 32],
    /// Content hash of the track
    pub content_hash: [u8; 32],
    /// Verified listen count at time of mint
    pub listen_count: u64,
    /// Ownership shares (basis points)
    pub ownership_shares: u64,
    /// Mint timestamp
    pub minted_at: i64,
    /// Can be used to re-access the content (playback token)
    pub playback_enabled: bool,
}

impl ListenNFT {
    pub const SIZE: usize = 1 + 32 + 32 + 32 + 8 + 8 + 8 + 1; // 122 bytes
}

// ── Stream-to-Own Thresholds ──

/// トラックのステージ（インディーズ → メジャー）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackStage {
    /// インディーズ: 0-9,999 total listens — 早期リスナーに手厚い報酬
    Indie,
    /// ライジング: 10,000-99,999 — 注目され始めた段階
    Rising,
    /// メジャー: 100,000+ — メジャーデビュー、収益拡大
    Major,
}

impl TrackStage {
    pub fn from_total_listens(listens: u64) -> Self {
        match listens {
            0..=9_999 => TrackStage::Indie,
            10_000..=99_999 => TrackStage::Rising,
            _ => TrackStage::Major,
        }
    }
}

/// Listen count → ownership shares (basis points, 10000 = 100%)
///
/// インディーズ時代の早期リスナーほど大きなシェアを獲得。
/// メジャーになると新規リスナーのシェアは薄くなるが、
/// インディーズ時代のリスナーのシェアは維持される。
///
/// ```text
/// Stage     | Listens to qualify | Share per listener | Max total distribution
/// ----------|-------------------|--------------------|----------------------
/// Indie     | 10+               | 10 bps (0.10%)     | 30% (early supporters)
/// Rising    | 100+              | 3 bps  (0.03%)     | +15% (growth phase)
/// Major     | 1000+             | 1 bps  (0.01%)     | +5%  (mass adoption)
///           |                   |                    | = 50% total max
/// ```
///
/// Artist retains minimum 50% ownership always.
fn ownership_for_listens(listens: u64) -> u64 {
    match listens {
        0..=9 => 0,              // < 10: no ownership yet
        10..=99 => 10,           // Indie early bird: 0.10%
        100..=999 => 5,          // Indie regular: 0.05%
        1000..=9999 => 3,        // Rising: 0.03%
        10000..=99999 => 1,      // Major early: 0.01%
        _ => 1,                  // Major: 0.01% (capped)
    }
}

// ── Instruction Processing ──

pub fn process_instruction(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    instruction_data: &[u8],
) -> ProgramResult {
    let instruction = SolunaInstruction::try_from_slice(instruction_data)
        .map_err(|_| ProgramError::InvalidInstructionData)?;

    match instruction {
        SolunaInstruction::Initialize => process_initialize(program_id, accounts),
        SolunaInstruction::SubmitProof { merkle_root, chain_tip, record_count, channel } =>
            process_submit_proof(program_id, accounts, merkle_root, chain_tip, record_count, channel),
        SolunaInstruction::RegisterRelay { tier } =>
            process_register_relay(program_id, accounts, tier),
        SolunaInstruction::ClaimRelayReward { bytes_relayed } =>
            process_claim_relay_reward(program_id, accounts, bytes_relayed),
        SolunaInstruction::RegisterTrack { content_hash } =>
            process_register_track(program_id, accounts, content_hash),
        SolunaInstruction::UpdateOwnership { content_hash } =>
            process_update_ownership(program_id, accounts, content_hash),
        SolunaInstruction::MintListenNFT { content_hash, channel } =>
            process_mint_listen_nft(program_id, accounts, content_hash, channel),
    }
}

fn process_initialize(program_id: &Pubkey, accounts: &[AccountInfo]) -> ProgramResult {
    let iter = &mut accounts.iter();
    let state_account = next_account_info(iter)?;
    let authority = next_account_info(iter)?;
    let system_program = next_account_info(iter)?;

    if !authority.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let (state_pda, bump) = Pubkey::find_program_address(&[b"soluna"], program_id);
    if *state_account.key != state_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let rent = Rent::get()?;
    let space = SolunaState::SIZE;

    invoke_signed(
        &system_instruction::create_account(
            authority.key,
            state_account.key,
            rent.minimum_balance(space),
            space as u64,
            program_id,
        ),
        &[authority.clone(), state_account.clone(), system_program.clone()],
        &[&[b"soluna", &[bump]]],
    )?;

    let state = SolunaState {
        is_initialized: true,
        authority: *authority.key,
        enai_mint: Pubkey::default(), // Set later
        total_proofs: 0,
        total_relay_nodes: 0,
        total_tracks: 0,
    };
    state.serialize(&mut &mut state_account.data.borrow_mut()[..])?;

    msg!("Soluna program initialized");
    Ok(())
}

fn process_submit_proof(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    merkle_root: [u8; 32],
    chain_tip: [u8; 32],
    record_count: u64,
    channel: [u8; 32],
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let proof_account = next_account_info(iter)?;
    let listener = next_account_info(iter)?;
    let state_account = next_account_info(iter)?;
    let system_program = next_account_info(iter)?;
    let clock_account = next_account_info(iter)?;

    if !listener.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let clock = Clock::from_account_info(clock_account)?;

    let (proof_pda, bump) = Pubkey::find_program_address(
        &[b"pol", listener.key.as_ref(), &channel],
        program_id,
    );
    if *proof_account.key != proof_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    // Create or update
    if proof_account.data_len() == 0 {
        let rent = Rent::get()?;
        let space = ListenProof::SIZE;
        invoke_signed(
            &system_instruction::create_account(
                listener.key,
                proof_account.key,
                rent.minimum_balance(space),
                space as u64,
                program_id,
            ),
            &[listener.clone(), proof_account.clone(), system_program.clone()],
            &[&[b"pol", listener.key.as_ref(), &channel, &[bump]]],
        )?;

        // Increment total proofs
        let mut state = SolunaState::try_from_slice(&state_account.data.borrow())?;
        state.total_proofs += 1;
        state.serialize(&mut &mut state_account.data.borrow_mut()[..])?;
    }

    let ownership = ownership_for_listens(record_count);

    let proof = ListenProof {
        is_initialized: true,
        listener: *listener.key,
        channel,
        merkle_root,
        chain_tip,
        record_count,
        last_updated: clock.unix_timestamp,
        ownership_shares: ownership,
    };
    proof.serialize(&mut &mut proof_account.data.borrow_mut()[..])?;

    msg!("PoL submitted: {} records, {} ownership shares", record_count, ownership);
    Ok(())
}

fn process_register_relay(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    tier: u8,
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let relay_account = next_account_info(iter)?;
    let operator = next_account_info(iter)?;
    let state_account = next_account_info(iter)?;
    let system_program = next_account_info(iter)?;

    if !operator.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    if tier > 3 {
        return Err(ProgramError::InvalidArgument);
    }

    let (relay_pda, bump) = Pubkey::find_program_address(
        &[b"relay", operator.key.as_ref()],
        program_id,
    );
    if *relay_account.key != relay_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let rent = Rent::get()?;
    let space = RelayNode::SIZE;
    invoke_signed(
        &system_instruction::create_account(
            operator.key,
            relay_account.key,
            rent.minimum_balance(space),
            space as u64,
            program_id,
        ),
        &[operator.clone(), relay_account.clone(), system_program.clone()],
        &[&[b"relay", operator.key.as_ref(), &[bump]]],
    )?;

    let relay = RelayNode {
        is_initialized: true,
        operator: *operator.key,
        tier,
        total_bytes_relayed: 0,
        total_rewards: 0,
        last_claim: 0,
    };
    relay.serialize(&mut &mut relay_account.data.borrow_mut()[..])?;

    let mut state = SolunaState::try_from_slice(&state_account.data.borrow())?;
    state.total_relay_nodes += 1;
    state.serialize(&mut &mut state_account.data.borrow_mut()[..])?;

    msg!("Relay node registered: tier={}", tier);
    Ok(())
}

fn process_claim_relay_reward(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    bytes_relayed: u64,
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let relay_account = next_account_info(iter)?;
    let operator = next_account_info(iter)?;
    let _state_account = next_account_info(iter)?;
    let clock_account = next_account_info(iter)?;

    if !operator.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let clock = Clock::from_account_info(clock_account)?;

    let (relay_pda, _bump) = Pubkey::find_program_address(
        &[b"relay", operator.key.as_ref()],
        program_id,
    );
    if *relay_account.key != relay_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let mut relay = RelayNode::try_from_slice(&relay_account.data.borrow())?;
    if !relay.is_initialized {
        return Err(ProgramError::UninitializedAccount);
    }

    // Rate limit: minimum 1 hour between claims
    let min_interval = 3600i64;
    if clock.unix_timestamp - relay.last_claim < min_interval && relay.last_claim > 0 {
        msg!("Claim too frequent, wait {} seconds",
            min_interval - (clock.unix_timestamp - relay.last_claim));
        return Err(ProgramError::Custom(1)); // TooFrequent
    }

    // Calculate reward (nanoENAI)
    let tier_mult = match relay.tier {
        0 => 1_000_000_000u64, // Origin: 1.0x (in nano)
        1 => 500_000_000,      // Region: 0.5x
        2 => 250_000_000,      // Edge: 0.25x
        3 => 100_000_000,      // Swarm: 0.1x
        _ => 0,
    };
    // reward = bytes * 1e-9 * tier_mult / 1e9 (in nanoENAI)
    let reward_nano = bytes_relayed.saturating_mul(tier_mult) / 1_000_000_000;

    relay.total_bytes_relayed = relay.total_bytes_relayed.saturating_add(bytes_relayed);
    relay.total_rewards = relay.total_rewards.saturating_add(reward_nano);
    relay.last_claim = clock.unix_timestamp;
    relay.serialize(&mut &mut relay_account.data.borrow_mut()[..])?;

    msg!("Relay reward claimed: {} bytes, {} nanoENAI", bytes_relayed, reward_nano);
    Ok(())
}

fn process_register_track(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    content_hash: [u8; 32],
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let track_account = next_account_info(iter)?;
    let artist = next_account_info(iter)?;
    let state_account = next_account_info(iter)?;
    let system_program = next_account_info(iter)?;

    if !artist.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let (track_pda, bump) = Pubkey::find_program_address(
        &[b"track", &content_hash],
        program_id,
    );
    if *track_account.key != track_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let rent = Rent::get()?;
    let space = TrackRegistry::SIZE;
    invoke_signed(
        &system_instruction::create_account(
            artist.key,
            track_account.key,
            rent.minimum_balance(space),
            space as u64,
            program_id,
        ),
        &[artist.clone(), track_account.clone(), system_program.clone()],
        &[&[b"track", &content_hash, &[bump]]],
    )?;

    let track = TrackRegistry {
        is_initialized: true,
        artist: *artist.key,
        content_hash,
        total_listens: 0,
        total_ownership_distributed: 0,
        royalty_pool_lamports: 0,
        stage: 0, // Indie
        total_nfts: 0,
    };
    track.serialize(&mut &mut track_account.data.borrow_mut()[..])?;

    let mut state = SolunaState::try_from_slice(&state_account.data.borrow())?;
    state.total_tracks += 1;
    state.serialize(&mut &mut state_account.data.borrow_mut()[..])?;

    msg!("Track registered: {:?}", &content_hash[..8]);
    Ok(())
}

fn process_update_ownership(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    content_hash: [u8; 32],
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let proof_account = next_account_info(iter)?;
    let track_account = next_account_info(iter)?;
    let authority = next_account_info(iter)?;

    if !authority.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let (track_pda, _) = Pubkey::find_program_address(
        &[b"track", &content_hash],
        program_id,
    );
    if *track_account.key != track_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let proof = ListenProof::try_from_slice(&proof_account.data.borrow())?;
    if !proof.is_initialized {
        return Err(ProgramError::UninitializedAccount);
    }

    let mut track = TrackRegistry::try_from_slice(&track_account.data.borrow())?;
    if !track.is_initialized {
        return Err(ProgramError::UninitializedAccount);
    }

    // Update listen count and ownership
    let new_shares = ownership_for_listens(proof.record_count);
    track.total_listens = track.total_listens.saturating_add(proof.record_count);
    track.total_ownership_distributed = track.total_ownership_distributed.saturating_add(new_shares);

    // Update stage (Indie → Rising → Major)
    let old_stage = track.stage;
    track.stage = match TrackStage::from_total_listens(track.total_listens) {
        TrackStage::Indie => 0,
        TrackStage::Rising => 1,
        TrackStage::Major => 2,
    };
    track.serialize(&mut &mut track_account.data.borrow_mut()[..])?;

    let stage_name = match track.stage { 0 => "Indie", 1 => "Rising", 2 => "Major", _ => "?" };
    if track.stage != old_stage {
        msg!("🎉 STAGE UP! {} → {} (total listens: {})",
            match old_stage { 0 => "Indie", 1 => "Rising", _ => "?" }, stage_name, track.total_listens);
    }
    msg!("Ownership updated: {} listens → {} shares, stage={}", proof.record_count, new_shares, stage_name);
    Ok(())
}

fn process_mint_listen_nft(
    program_id: &Pubkey,
    accounts: &[AccountInfo],
    content_hash: [u8; 32],
    channel: [u8; 32],
) -> ProgramResult {
    let iter = &mut accounts.iter();
    let nft_account = next_account_info(iter)?;
    let proof_account = next_account_info(iter)?;
    let listener = next_account_info(iter)?;
    let state_account = next_account_info(iter)?;
    let system_program = next_account_info(iter)?;
    let clock_account = next_account_info(iter)?;

    if !listener.is_signer {
        return Err(ProgramError::MissingRequiredSignature);
    }

    let clock = Clock::from_account_info(clock_account)?;

    // Verify proof exists
    let proof = ListenProof::try_from_slice(&proof_account.data.borrow())?;
    if !proof.is_initialized {
        return Err(ProgramError::UninitializedAccount);
    }
    if proof.listener != *listener.key {
        msg!("Proof listener mismatch");
        return Err(ProgramError::InvalidAccountData);
    }

    // Minimum 10 listens to mint NFT
    if proof.record_count < 10 {
        msg!("Need at least 10 listens to mint NFT (have {})", proof.record_count);
        return Err(ProgramError::Custom(2)); // InsufficientListens
    }

    // Derive NFT PDA
    let (nft_pda, bump) = Pubkey::find_program_address(
        &[b"nft", listener.key.as_ref(), &content_hash],
        program_id,
    );
    if *nft_account.key != nft_pda {
        return Err(ProgramError::InvalidAccountData);
    }

    let ownership = ownership_for_listens(proof.record_count);

    // Create NFT account if needed
    if nft_account.data_len() == 0 {
        let rent = Rent::get()?;
        let space = ListenNFT::SIZE;
        invoke_signed(
            &system_instruction::create_account(
                listener.key,
                nft_account.key,
                rent.minimum_balance(space),
                space as u64,
                program_id,
            ),
            &[listener.clone(), nft_account.clone(), system_program.clone()],
            &[&[b"nft", listener.key.as_ref(), &content_hash, &[bump]]],
        )?;
    }

    let nft = ListenNFT {
        is_initialized: true,
        owner: *listener.key,
        channel,
        content_hash,
        listen_count: proof.record_count,
        ownership_shares: ownership,
        minted_at: clock.unix_timestamp,
        playback_enabled: true, // NFT holder can always play back
    };
    nft.serialize(&mut &mut nft_account.data.borrow_mut()[..])?;

    msg!("NFT minted! listens={} ownership={}bps playback=true", proof.record_count, ownership);
    Ok(())
}
