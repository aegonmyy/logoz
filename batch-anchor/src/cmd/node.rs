use anyhow::{Context, Result};
use clap::Subcommand;
use std::path::Path;
use std::process::{Command, Stdio};

use crate::config::Config;

#[derive(Subcommand, Debug, Clone)]
pub enum NodeAction {
    /// Start the nwaku node (idempotent — no-op if already running).
    Up,
    /// Stop and remove the nwaku container (the SQLite store persists).
    Down,
    /// Show container state via `docker compose ps`.
    Status,
    /// Tail nwaku logs. Ctrl-C to detach.
    Logs,
}

pub fn run(cfg: Config, action: NodeAction) -> Result<()> {
    let compose_file = &cfg.node.compose_file;
    if !compose_file.exists() {
        anyhow::bail!(
            "compose file not found at {} (relative to cwd)",
            compose_file.display()
        );
    }

    match action {
        NodeAction::Up => compose(compose_file, &["up", "-d"]),
        NodeAction::Down => compose(compose_file, &["down"]),
        NodeAction::Status => compose(compose_file, &["ps"]),
        NodeAction::Logs => compose(compose_file, &["logs", "-f"]),
    }
}

fn compose(compose_file: &Path, args: &[&str]) -> Result<()> {
    let mut cmd = Command::new("docker");
    cmd.arg("compose")
        .arg("-f")
        .arg(compose_file)
        .args(args)
        .stdin(Stdio::inherit())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());

    let status = cmd
        .status()
        .context("running `docker compose` — is docker installed and on $PATH?")?;
    if !status.success() {
        anyhow::bail!(
            "`docker compose {}` failed: exit {}",
            args.join(" "),
            status
        );
    }
    Ok(())
}
