use anyhow::Result;
use std::process::ExitCode;

use crate::config::Config;
use crate::registry::RegistryClient;

pub async fn run(cfg: Config, cid: String) -> Result<ExitCode> {
    let registry = RegistryClient::new(&cfg.registry)?;
    let cid_clone = cid.clone();
    let maybe_reg = tokio::task::spawn_blocking(move || registry.fetch_registry()).await??;

    let reg = match maybe_reg {
        Some(r) => r,
        None => {
            eprintln!("Registry PDA not found — run `batch-anchor init` first");
            return Ok(ExitCode::from(2));
        }
    };

    match reg.entries.get(&cid_clone) {
        Some(rec) => {
            println!("CID:               {}", cid_clone);
            println!("metadata_hash:     {}", hex::encode(rec.metadata_hash));
            println!("anchor_timestamp:  {}", rec.anchor_timestamp);
            println!("anchored_by:       {}", hex::encode(rec.anchored_by));
            println!("version:           {}", rec.version);
            Ok(ExitCode::SUCCESS)
        }
        None => {
            println!("{}: not registered", cid_clone);
            Ok(ExitCode::from(1))
        }
    }
}
