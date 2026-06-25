#![no_main]

use spel_framework::prelude::*;
use nssa_core::account::Data;

use chronicle_registry_core::{
    CidRecord, Registry, MAX_BATCH,
    E_ARITY_MISMATCH, E_BAD_TIMESTAMP, E_BATCH_EMPTY, E_BATCH_TOO_BIG,
    E_INVALID_HASH, E_REGISTRY_FULL,
};

risc0_zkvm::guest::entry!(main);

#[lez_program]
mod chronicle_registry {
    #[allow(unused_imports)]
    use super::*;

    /// Open the registry. Permissionless — anyone can call this once.
    #[instruction]
    pub fn init_registry(
        #[account(init, pda = [literal("registry")])]
        mut registry: AccountWithMetadata,
        #[account(signer)]
        anchorer: AccountWithMetadata,
    ) -> SpelResult {
        let cu_start = risc0_zkvm::guest::env::cycle_count();
        let empty = Registry::default();
        let bytes = borsh::to_vec(&empty)
            .map_err(|e| SpelError::SerializationError { message: e.to_string() })?;
        registry.account.data = Data::try_from(bytes)
            .map_err(|_| SpelError::custom(E_REGISTRY_FULL, "registry bytes overflow".to_string()))?;
        let cu_end = risc0_zkvm::guest::env::cycle_count();
        risc0_zkvm::guest::env::log(&format!("CU init_registry cycles={}", cu_end - cu_start));
        Ok(SpelOutput::execute(vec![registry, anchorer], vec![]))
    }

    /// Anchor a batch of CIDs (up to MAX_BATCH=50). Idempotent — duplicates
    /// are silently skipped. Three parallel vecs must have equal length.
    #[instruction]
    pub fn index_batch(
        #[account(mut, pda = [literal("registry")])]
        mut registry: AccountWithMetadata,
        #[account(signer)]
        anchorer: AccountWithMetadata,
        cids: Vec<String>,
        metadata_hashes: Vec<[u8; 32]>,
        anchor_timestamps: Vec<u32>,
    ) -> SpelResult {
        let cu_start = risc0_zkvm::guest::env::cycle_count();
        let n = cids.len();
        if n == 0 {
            return Err(SpelError::custom(E_BATCH_EMPTY, "batch is empty".to_string()));
        }
        if n > MAX_BATCH {
            return Err(SpelError::custom(
                E_BATCH_TOO_BIG,
                format!("batch size {} > MAX_BATCH {}", n, MAX_BATCH),
            ));
        }
        if metadata_hashes.len() != n || anchor_timestamps.len() != n {
            return Err(SpelError::custom(
                E_ARITY_MISMATCH,
                format!(
                    "cids={}, metadata_hashes={}, anchor_timestamps={} (must all match)",
                    n, metadata_hashes.len(), anchor_timestamps.len()
                ),
            ));
        }

        let anchorer_id = *anchorer.account_id.value();
        for i in 0..n {
            if cids[i].is_empty() || metadata_hashes[i] == [0u8; 32] {
                return Err(SpelError::custom(E_INVALID_HASH, "cid empty or metadata_hash all-zero".to_string()));
            }
            if anchor_timestamps[i] == 0 {
                return Err(SpelError::custom(E_BAD_TIMESTAMP, "anchor_timestamp == 0".to_string()));
            }
        }

        let mut state = if registry.account.data.is_empty() {
            Registry::default()
        } else {
            borsh::from_slice::<Registry>(&registry.account.data)
                .map_err(|e| SpelError::SerializationError { message: e.to_string() })?
        };

        for (i, cid) in cids.into_iter().enumerate() {
            if state.entries.contains_key(&cid) {
                continue;
            }
            state.entries.insert(
                cid,
                CidRecord {
                    metadata_hash: metadata_hashes[i],
                    anchor_timestamp: anchor_timestamps[i] as i64,
                    anchored_by: anchorer_id,
                    version: 1,
                },
            );
        }

        let bytes = borsh::to_vec(&state)
            .map_err(|e| SpelError::SerializationError { message: e.to_string() })?;
        registry.account.data = Data::try_from(bytes)
            .map_err(|_| SpelError::custom(E_REGISTRY_FULL, "registry would exceed 100 KiB".to_string()))?;

        let cu_end = risc0_zkvm::guest::env::cycle_count();
        risc0_zkvm::guest::env::log(&format!("CU index_batch n={} cycles={}", n, cu_end - cu_start));
        Ok(SpelOutput::execute(vec![registry, anchorer], vec![]))
    }
}
