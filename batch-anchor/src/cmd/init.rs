use anyhow::Result;
use tracing::info;

use crate::config::Config;
use crate::registry::RegistryClient;

pub async fn run(cfg: Config) -> Result<()> {
    let registry = RegistryClient::new(&cfg.registry)?;

    info!("Calling init_registry ...");
    let was_initialised = tokio::task::spawn_blocking(move || registry.init_registry()).await??;

    if was_initialised {
        info!("Registry initialised");
        println!("Registry initialised");
    } else {
        info!("Registry already initialised (no-op)");
        println!("Registry already initialised");
    }
    Ok(())
}
