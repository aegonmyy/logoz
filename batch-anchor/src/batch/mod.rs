use std::collections::HashSet;
use std::time::Instant;

use crate::delivery::Envelope;

pub const MAX_BATCH: usize = 50;

#[derive(Debug, Clone)]
pub struct PendingEntry {
    pub cid: String,
    pub metadata_hash: [u8; 32],
    pub timestamp: u64,
}

pub struct BatchBuffer {
    entries: Vec<PendingEntry>,
    first_entry_at: Option<Instant>,
    max_size: usize,
    flush_after_s: u64,
    pub known: HashSet<String>,
}

impl BatchBuffer {
    pub fn new(max_size: usize, flush_after_s: u64, known: HashSet<String>) -> Self {
        Self {
            entries: Vec::new(),
            first_entry_at: None,
            max_size,
            flush_after_s,
            known,
        }
    }

    pub fn push(&mut self, env: Envelope) {
        if self.known.contains(&env.cid) {
            return;
        }
        if self.entries.iter().any(|e| e.cid == env.cid) {
            return;
        }
        if self.first_entry_at.is_none() {
            self.first_entry_at = Some(Instant::now());
        }
        self.entries.push(PendingEntry {
            cid: env.cid,
            metadata_hash: env.metadata_hash,
            timestamp: env.timestamp,
        });
    }

    pub fn should_flush(&self) -> bool {
        if self.entries.is_empty() {
            return false;
        }
        if self.entries.len() >= self.max_size {
            return true;
        }
        if let Some(first) = self.first_entry_at {
            return first.elapsed().as_secs() >= self.flush_after_s;
        }
        false
    }

    pub fn drain(&mut self) -> Vec<PendingEntry> {
        self.first_entry_at = None;
        std::mem::take(&mut self.entries)
    }

    pub fn mark_flushed(&mut self, entries: &[PendingEntry]) {
        for e in entries {
            self.known.insert(e.cid.clone());
        }
    }

    pub fn return_failed(&mut self, mut entries: Vec<PendingEntry>) {
        if self.first_entry_at.is_none() && !entries.is_empty() {
            self.first_entry_at = Some(Instant::now());
        }
        entries.append(&mut self.entries);
        self.entries = entries;
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn env(cid: &str) -> Envelope {
        Envelope {
            cid: cid.to_string(),
            metadata_hash: [0u8; 32],
            timestamp: 1,
            metadata: serde_json::Value::Null,
        }
    }

    #[test]
    fn skips_known_cids() {
        let known = HashSet::from(["bafy1".to_string()]);
        let mut buf = BatchBuffer::new(50, 30, known);
        buf.push(env("bafy1"));
        assert_eq!(buf.len(), 0);
    }

    #[test]
    fn skips_in_buffer_duplicates() {
        let mut buf = BatchBuffer::new(50, 30, HashSet::new());
        buf.push(env("bafy1"));
        buf.push(env("bafy1"));
        assert_eq!(buf.len(), 1);
    }

    #[test]
    fn flushes_at_max_size() {
        let mut buf = BatchBuffer::new(3, 9999, HashSet::new());
        buf.push(env("a"));
        buf.push(env("b"));
        assert!(!buf.should_flush());
        buf.push(env("c"));
        assert!(buf.should_flush());
    }

    #[test]
    fn drain_clears_entries() {
        let mut buf = BatchBuffer::new(50, 30, HashSet::new());
        buf.push(env("a"));
        let drained = buf.drain();
        assert_eq!(drained.len(), 1);
        assert!(buf.is_empty());
    }

    #[test]
    fn mark_flushed_updates_known_set() {
        let mut buf = BatchBuffer::new(50, 30, HashSet::new());
        buf.push(env("a"));
        let batch = buf.drain();
        buf.mark_flushed(&batch);
        buf.push(env("a"));
        assert!(buf.is_empty());
    }

    #[test]
    fn return_failed_puts_entries_back() {
        let mut buf = BatchBuffer::new(50, 30, HashSet::new());
        buf.push(env("a"));
        let batch = buf.drain();
        assert!(buf.is_empty());
        buf.return_failed(batch);
        assert_eq!(buf.len(), 1);
    }
}
