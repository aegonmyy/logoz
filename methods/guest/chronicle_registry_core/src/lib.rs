use std::collections::HashMap;

use borsh::{BorshDeserialize, BorshSerialize};
use serde::{Deserialize, Serialize};

/// Maximum CIDs accepted in a single `index_batch` call.
/// Matches LP-0017 R8's 50-CID batch benchmark requirement.
pub const MAX_BATCH: usize = 50;

/// One on-chain record per anchored document.
/// The CID is the map key in [`Registry::entries`].
#[derive(
    Debug, Clone, Default, PartialEq, Eq,
    Serialize, Deserialize,
    BorshSerialize, BorshDeserialize,
)]
pub struct CidRecord {
    /// SHA-256 of the metadata envelope JSON.
    pub metadata_hash: [u8; 32],
    /// Unix seconds at time of anchoring (stored as i64 for LEZ compatibility).
    pub anchor_timestamp: i64,
    /// Account ID of the anchorer (32-byte public key).
    pub anchored_by: [u8; 32],
    /// Schema version — increment if CidRecord fields change.
    pub version: u8,
}

/// Global registry state held in the PDA at seed `[b"registry"]`.
///
/// `HashMap` serialized by borsh uses sorted keys, so on-chain bytes are
/// deterministic across guest executions regardless of insertion order.
#[derive(Debug, Clone, Default, Serialize, Deserialize, BorshSerialize, BorshDeserialize)]
pub struct Registry {
    pub entries: HashMap<String, CidRecord>,
}

// ── Error codes ────────────────────────────────────────────────────────────

pub const E_INVALID_CID: u32 = 1;
pub const E_BAD_TIMESTAMP: u32 = 2;
pub const E_BATCH_EMPTY: u32 = 3;
pub const E_BATCH_TOO_BIG: u32 = 4;
pub const E_REGISTRY_FULL: u32 = 5;
pub const E_ARITY_MISMATCH: u32 = 6;
pub const E_INVALID_HASH: u32 = 7;
