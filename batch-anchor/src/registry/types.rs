use borsh::{BorshDeserialize, BorshSerialize};
use std::collections::HashMap;

#[derive(Debug, BorshDeserialize, BorshSerialize)]
pub struct Registry {
    pub entries: HashMap<String, CidRecord>,
}

#[derive(Debug, Clone, BorshDeserialize, BorshSerialize)]
pub struct CidRecord {
    pub metadata_hash: [u8; 32],
    pub anchor_timestamp: i64,
    pub anchored_by: [u8; 32],
    pub version: u8,
}

#[cfg(test)]
mod tests {
    use super::*;
    use borsh::BorshDeserialize;

    #[test]
    fn empty_registry_roundtrips() {
        let reg = Registry {
            entries: HashMap::new(),
        };
        let bytes = borsh::to_vec(&reg).unwrap();
        let decoded = Registry::try_from_slice(&bytes).unwrap();
        assert_eq!(decoded.entries.len(), 0);
    }

    #[test]
    fn registry_with_one_entry_roundtrips() {
        let mut entries = HashMap::new();
        entries.insert(
            "bafyTestCid".to_string(),
            CidRecord {
                metadata_hash: [0xab; 32],
                anchor_timestamp: 1715000000,
                anchored_by: [0xcd; 32],
                version: 1,
            },
        );
        let reg = Registry { entries };
        let bytes = borsh::to_vec(&reg).unwrap();
        let decoded = Registry::try_from_slice(&bytes).unwrap();
        let rec = decoded.entries.get("bafyTestCid").unwrap();
        assert_eq!(rec.metadata_hash, [0xab; 32]);
        assert_eq!(rec.anchor_timestamp, 1715000000);
        assert_eq!(rec.version, 1);
    }
}
