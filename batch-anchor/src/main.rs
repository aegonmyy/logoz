mod batch;
mod cmd;
mod config;
mod delivery;
mod registry;

use anyhow::Result;
use clap::{Parser, Subcommand};
use config::Config;
use std::path::PathBuf;
use std::process::ExitCode;

/// batch-anchor — batch anchor CIDs broadcast over Logos Delivery to the
/// chronicle-registry LEZ program.
#[derive(Parser)]
#[command(version, about, long_about = None)]
struct Cli {
    /// Path to the TOML config file.
    #[arg(short, long, global = true, default_value = "batch-anchor.toml")]
    config: PathBuf,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Subscribe to the Delivery topic and anchor incoming CIDs in batches.
    Watch,
    /// Open the on-chain registry (idempotent — safe to re-run).
    Init,
    /// Look up a single CID on-chain. Exits non-zero if not registered.
    Lookup {
        /// The CID to query.
        cid: String,
    },
    /// Manage the local nwaku node via docker compose.
    Node {
        #[command(subcommand)]
        action: cmd::node::NodeAction,
    },
}

#[tokio::main]
async fn main() -> Result<ExitCode> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .init();

    let cli = Cli::parse();
    let cfg = Config::load(&cli.config)?;

    match cli.command {
        Command::Watch => {
            cmd::watch::run(cfg).await?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Init => {
            cmd::init::run(cfg).await?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Lookup { cid } => cmd::lookup::run(cfg, cid).await,
        Command::Node { action } => {
            cmd::node::run(cfg, action)?;
            Ok(ExitCode::SUCCESS)
        }
    }
}
