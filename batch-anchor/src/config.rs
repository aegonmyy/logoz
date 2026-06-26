use anyhow::{Context, Result};
use serde::Deserialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Deserialize)]
pub struct Config {
    pub delivery: DeliveryConfig,
    pub registry: RegistryConfig,
    pub batch: BatchConfig,
    pub node: NodeConfig,
}

#[derive(Debug, Deserialize)]
pub struct NodeConfig {
    pub compose_file: PathBuf,
}

#[derive(Debug, Deserialize)]
pub struct DeliveryConfig {
    pub rest_url: String,
    pub content_topic: String,
    pub poll_ms: u64,
    pub store_lookback_hours: u64,
}

#[derive(Debug, Deserialize)]
pub struct RegistryConfig {
    pub spel_toml: PathBuf,
    pub sequencer_url: String,
    pub wallet_home: PathBuf,
    pub signer_account_id: String,
    pub program_id: String,
}

#[derive(Debug, Deserialize)]
pub struct BatchConfig {
    pub max_size: usize,
    pub flush_after_s: u64,
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let raw = std::fs::read_to_string(path)
            .with_context(|| format!("reading config {}", path.display()))?;
        let mut cfg: Config =
            toml::from_str(&raw).with_context(|| format!("parsing config {}", path.display()))?;
        cfg.apply_env();
        Ok(cfg)
    }

    fn apply_env(&mut self) {
        if let Ok(v) = std::env::var("BATCH_ANCHOR_DELIVERY__REST_URL") {
            self.delivery.rest_url = v;
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_DELIVERY__CONTENT_TOPIC") {
            self.delivery.content_topic = v;
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_DELIVERY__POLL_MS") {
            if let Ok(n) = v.parse() {
                self.delivery.poll_ms = n;
            }
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_DELIVERY__STORE_LOOKBACK_HOURS") {
            if let Ok(n) = v.parse() {
                self.delivery.store_lookback_hours = n;
            }
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_REGISTRY__SEQUENCER_URL") {
            self.registry.sequencer_url = v;
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_REGISTRY__WALLET_HOME") {
            self.registry.wallet_home = PathBuf::from(v);
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_REGISTRY__SIGNER_ACCOUNT_ID") {
            self.registry.signer_account_id = v;
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_REGISTRY__PROGRAM_ID") {
            self.registry.program_id = v;
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_BATCH__MAX_SIZE") {
            if let Ok(n) = v.parse() {
                self.batch.max_size = n;
            }
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_BATCH__FLUSH_AFTER_S") {
            if let Ok(n) = v.parse() {
                self.batch.flush_after_s = n;
            }
        }
        if let Ok(v) = std::env::var("BATCH_ANCHOR_NODE__COMPOSE_FILE") {
            self.node.compose_file = PathBuf::from(v);
        }
    }
}
