use anyhow::Result;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tracing::{info, warn};

use crate::batch::BatchBuffer;
use crate::config::Config;
use crate::delivery::DeliveryClient;
use crate::registry::RegistryClient;

pub async fn run(cfg: Config) -> Result<()> {
    let registry = RegistryClient::new(&cfg.registry)?;

    info!("Seeding dedup set from on-chain registry ...");
    let known = {
        let r = registry.clone();
        tokio::task::spawn_blocking(move || r.anchored_cid_set()).await??
    };
    info!("{} CIDs already anchored on-chain", known.len());

    let mut buffer = BatchBuffer::new(cfg.batch.max_size, cfg.batch.flush_after_s, known);
    let delivery = DeliveryClient::new(&cfg.delivery.rest_url);

    info!("Waiting for nwaku at {} ...", cfg.delivery.rest_url);
    for attempt in 1..=15 {
        if delivery.healthy().await {
            break;
        }
        if attempt == 15 {
            anyhow::bail!("nwaku not reachable — is docker-compose up?");
        }
        tokio::time::sleep(Duration::from_secs(2)).await;
    }
    info!("nwaku ready");

    if cfg.delivery.store_lookback_hours > 0 {
        let start_ns = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
            .saturating_sub((cfg.delivery.store_lookback_hours as u128) * 3_600 * 1_000_000_000);

        info!(
            "Catching up via store-protocol (last {}h) ...",
            cfg.delivery.store_lookback_hours
        );
        match delivery
            .query_store(&cfg.delivery.content_topic, start_ns)
            .await
        {
            Ok(envelopes) => {
                let mut new = 0usize;
                let total = envelopes.len();
                for env in envelopes {
                    let before = buffer.len();
                    buffer.push(env);
                    if buffer.len() > before {
                        new += 1;
                    }
                }
                info!(
                    "Catch-up: {} historical message(s) seen, {} new to anchor",
                    total, new
                );
            }
            Err(e) => warn!("store catch-up failed (continuing with live subscribe): {e:#}"),
        }
    }

    delivery.subscribe(&cfg.delivery.content_topic).await?;
    info!("Subscribed to {}", cfg.delivery.content_topic);
    info!(
        "Polling every {}ms — send broadcasts now ...",
        cfg.delivery.poll_ms
    );

    loop {
        tokio::time::sleep(Duration::from_millis(cfg.delivery.poll_ms)).await;

        let envelopes = match delivery.drain(&cfg.delivery.content_topic).await {
            Ok(e) => e,
            Err(e) => {
                warn!("poll error: {e}");
                continue;
            }
        };

        for env in envelopes {
            info!(
                cid = %env.cid,
                metadata_hash = %env.metadata_hash_hex(),
                metadata = %env.metadata,
                "envelope received"
            );
            buffer.push(env);
        }

        if buffer.should_flush() {
            let batch = buffer.drain();
            info!("FLUSH: anchoring {} CID(s) ...", batch.len());

            let r = registry.clone();
            let to_submit = batch.clone();
            let result = tokio::task::spawn_blocking(move || r.index_batch(&to_submit)).await?;

            match result {
                Ok(()) => {
                    info!("Anchored {} CID(s) on-chain", batch.len());
                    buffer.mark_flushed(&batch);
                }
                Err(e) => {
                    warn!("index_batch failed: {e:#} — returning batch to buffer");
                    buffer.return_failed(batch);
                }
            }
        }
    }
}
