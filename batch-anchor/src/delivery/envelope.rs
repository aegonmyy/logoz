use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde::Deserialize;

/// Parsed, validated broadcast envelope from the Logos Delivery topic.
#[derive(Debug, Clone)]
pub struct Envelope {
    pub cid: String,
    pub metadata_hash: [u8; 32],
    pub timestamp: u64,
    pub metadata: serde_json::Value,
}

#[derive(Deserialize)]
struct RawEnvelope {
    cid: String,
    metadata_hash: String,
    timestamp: u64,
    v: u8,
}

impl Envelope {
    pub fn from_payload(payload_b64: &str) -> Result<Self> {
        let bytes = B64.decode(payload_b64).context("base64 decode payload")?;
        let metadata: serde_json::Value =
            serde_json::from_slice(&bytes).context("JSON parse envelope")?;
        let raw: RawEnvelope =
            serde_json::from_value(metadata.clone()).context("JSON parse envelope")?;

        if raw.v != 1 {
            bail!("unsupported envelope version: {}", raw.v);
        }
        if raw.cid.is_empty() {
            bail!("empty CID in envelope");
        }
        if raw.timestamp == 0 {
            bail!("zero timestamp in envelope");
        }

        let metadata_hash = parse_metadata_hash(&raw.metadata_hash)
            .with_context(|| format!("invalid metadata_hash: {}", raw.metadata_hash))?;

        Ok(Self {
            cid: raw.cid,
            metadata_hash,
            timestamp: raw.timestamp,
            metadata,
        })
    }

    pub fn metadata_hash_hex(&self) -> String {
        hex::encode(self.metadata_hash)
    }
}

fn parse_metadata_hash(s: &str) -> Result<[u8; 32]> {
    let hex_part = s
        .strip_prefix("v1:")
        .with_context(|| format!("expected \"v1:\" prefix, got: {s}"))?;
    let bytes =
        hex::decode(hex_part).with_context(|| format!("hex decode failed for: {hex_part}"))?;
    bytes
        .try_into()
        .map_err(|v: Vec<u8>| anyhow::anyhow!("expected 32 bytes, got {}", v.len()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::{engine::general_purpose::STANDARD as B64, Engine};

    fn make_payload(cid: &str, meta: &str, ts: u64, v: u8) -> String {
        let json = serde_json::json!({
            "cid": cid,
            "metadata_hash": meta,
            "timestamp": ts,
            "v": v,
            "title": "test",
        });
        B64.encode(json.to_string())
    }

    #[test]
    fn valid_envelope_roundtrips() {
        let hash_hex = "a".repeat(64);
        let payload = make_payload("bafybeifoo", &format!("v1:{hash_hex}"), 1715000000, 1);
        let env = Envelope::from_payload(&payload).unwrap();
        assert_eq!(env.cid, "bafybeifoo");
        assert_eq!(env.metadata_hash_hex(), hash_hex);
        assert_eq!(env.timestamp, 1715000000);
    }

    #[test]
    fn rejects_wrong_version() {
        let payload = make_payload("bafybeifoo", &format!("v1:{}", "a".repeat(64)), 1, 2);
        assert!(Envelope::from_payload(&payload).is_err());
    }

    #[test]
    fn rejects_empty_cid() {
        let payload = make_payload("", &format!("v1:{}", "a".repeat(64)), 1, 1);
        assert!(Envelope::from_payload(&payload).is_err());
    }

    #[test]
    fn rejects_missing_v1_prefix() {
        let payload = make_payload("bafybeifoo", &"a".repeat(64), 1, 1);
        assert!(Envelope::from_payload(&payload).is_err());
    }

    #[test]
    fn rejects_wrong_hash_length() {
        let payload = make_payload("bafybeifoo", "v1:deadbeef", 1, 1);
        assert!(Envelope::from_payload(&payload).is_err());
    }
}
