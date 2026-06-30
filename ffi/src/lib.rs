//! C FFI for the chronicle-registry SPEL program.
//!
//! Every exported function accepts a JSON string and returns a heap-allocated
//! JSON string. The caller must free returned strings with
//! `chronicle_registry_free_string`.
//!
//! Functions shell out to the auto-generated `chronicle_registry_cli` binary
//! (via `spel` CLI) so that SPEL/nssa version details are encapsulated in the
//! compiled binary and not duplicated here.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::process::Command;

use borsh::BorshDeserialize;
use serde_json::{json, Value};

use chronicle_registry_core::{CidRecord, Registry, MAX_BATCH};

// ─── Helpers ─────────────────────────────────────────────────────────────────

unsafe fn cstr_to_str<'a>(ptr: *const c_char) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err("null pointer".into());
    }
    CStr::from_ptr(ptr)
        .to_str()
        .map_err(|e| format!("invalid UTF-8: {}", e))
}

fn to_cstring(s: String) -> *mut c_char {
    CString::new(s)
        .unwrap_or_else(|_| {
            CString::new(r#"{"ok":false,"error":"null byte in output"}"#).unwrap()
        })
        .into_raw()
}

fn error_json(msg: &str) -> *mut c_char {
    let v = serde_json::json!(msg).to_string();
    to_cstring(format!(r#"{{"ok":false,"error":{}}}"#, v))
}

fn ffi_call(
    f: impl FnOnce() -> Result<String, String> + std::panic::UnwindSafe,
) -> *mut c_char {
    match std::panic::catch_unwind(f) {
        Ok(Ok(r)) => to_cstring(r),
        Ok(Err(e)) => error_json(&e),
        Err(e) => {
            let msg = e
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| e.downcast_ref::<String>().map(|s| s.as_str()))
                .unwrap_or("<unknown panic>");
            error_json(&format!("panic: {}", msg))
        }
    }
}

/// Build a `spel` command with `-i <idl> -p <program_id>` flags.
/// The caller appends `-- <subcommand> [args...]`.
fn spel_cmd(v: &Value) -> Result<Command, String> {
    let idl = v["idl_path"]
        .as_str()
        .ok_or("missing idl_path")?;
    let program_id = v["program_id_hex"]
        .as_str()
        .ok_or("missing program_id_hex")?;
    if program_id.len() != 64 || !program_id.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(format!(
            "program_id_hex must be 64 hex chars, got {}",
            program_id.len()
        ));
    }
    let mut cmd = Command::new("spel");
    cmd.arg("-i").arg(idl).arg("-p").arg(program_id);

    if let Some(wallet_home) = v["wallet_home"].as_str() {
        cmd.env("LEE_WALLET_HOME_DIR", wallet_home);
    }
    // LEZ rc5 takes the sequencer endpoint from the wallet home's
    // wallet_config.json ("sequencer_addr"), not an env var — the old
    // NSSA_SEQUENCER_URL is ignored. The "sequencer_url" arg is accepted for
    // backward compatibility but has no effect; configure it in the wallet.
    let _ = v["sequencer_url"].as_str();
    Ok(cmd)
}

fn run_spel(mut cmd: Command, args: &[&str]) -> Result<String, String> {
    let output = cmd
        .arg("--")
        .args(args)
        .output()
        .map_err(|e| format!("failed to spawn spel: {}", e))?;

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();

    if !output.status.success() {
        return Err(format!(
            "spel exited {}: {}{}",
            output.status,
            stdout,
            stderr
        ));
    }
    Ok(stdout)
}

// ─── init_registry ───────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn chronicle_registry_init_registry(args_json: *const c_char) -> *mut c_char {
    let args = match unsafe { cstr_to_str(args_json) } {
        Ok(s) => s.to_owned(),
        Err(e) => return error_json(&e),
    };
    ffi_call(move || init_registry_impl(&args))
}

fn init_registry_impl(args: &str) -> Result<String, String> {
    let v: Value = serde_json::from_str(args).map_err(|e| format!("invalid JSON: {}", e))?;
    let anchorer = v["anchorer"].as_str().ok_or("missing anchorer")?;
    let cmd = spel_cmd(&v)?;
    let out = run_spel(cmd, &["init-registry", "--anchorer", anchorer])?;
    Ok(json!({"ok": true, "output": out.trim()}).to_string())
}

// ─── index_batch ─────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn chronicle_registry_index_batch(args_json: *const c_char) -> *mut c_char {
    let args = match unsafe { cstr_to_str(args_json) } {
        Ok(s) => s.to_owned(),
        Err(e) => return error_json(&e),
    };
    ffi_call(move || index_batch_impl(&args))
}

fn index_batch_impl(args: &str) -> Result<String, String> {
    let v: Value = serde_json::from_str(args).map_err(|e| format!("invalid JSON: {}", e))?;
    let anchorer = v["anchorer"].as_str().ok_or("missing anchorer")?;
    let entries = v["entries"].as_array().ok_or("missing entries array")?;
    if entries.is_empty() {
        return Err("empty batch".into());
    }
    if entries.len() > MAX_BATCH {
        return Err(format!("{} exceeds MAX_BATCH={}", entries.len(), MAX_BATCH));
    }

    let mut cmd = spel_cmd(&v)?;
    let mut sub_args: Vec<String> = vec![
        "index-batch".to_string(),
        "--anchorer".to_string(),
        anchorer.to_string(),
    ];

    let mut hashes = Vec::with_capacity(entries.len());
    let mut timestamps = Vec::with_capacity(entries.len());

    for (i, e) in entries.iter().enumerate() {
        let cid = e["cid"]
            .as_str()
            .ok_or_else(|| format!("entries[{}].cid missing", i))?;
        if cid.is_empty() {
            return Err(format!("entries[{}].cid empty", i));
        }
        sub_args.push("--cids".to_string());
        sub_args.push(cid.to_string());

        let hash_str = e["metadata_hash"]
            .as_str()
            .ok_or_else(|| format!("entries[{}].metadata_hash missing", i))?
            .trim_start_matches("0x");
        let hash_bytes = hex::decode(hash_str)
            .map_err(|e| format!("entries[{}].metadata_hash invalid hex: {}", i, e))?;
        if hash_bytes.len() != 32 {
            return Err(format!(
                "entries[{}].metadata_hash must be 32 bytes (got {})",
                i,
                hash_bytes.len()
            ));
        }
        hashes.push(hash_str.to_lowercase());

        let ts = e["timestamp"]
            .as_u64()
            .ok_or_else(|| format!("entries[{}].timestamp missing", i))?;
        if ts == 0 || ts > u32::MAX as u64 {
            return Err(format!("entries[{}].timestamp out of range", i));
        }
        timestamps.push(ts.to_string());
    }

    sub_args.extend_from_slice(&[
        "--metadata-hashes".to_string(),
        hashes.join(","),
        "--anchor-timestamps".to_string(),
        timestamps.join(","),
    ]);

    let refs: Vec<&str> = sub_args.iter().map(|s| s.as_str()).collect();
    let out = run_spel(cmd, &refs)?;
    Ok(json!({"ok": true, "output": out.trim()}).to_string())
}

// ─── get_registry ────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn chronicle_registry_get_registry(args_json: *const c_char) -> *mut c_char {
    let args = match unsafe { cstr_to_str(args_json) } {
        Ok(s) => s.to_owned(),
        Err(e) => return error_json(&e),
    };
    ffi_call(move || get_registry_impl(&args))
}

fn get_registry_impl(args: &str) -> Result<String, String> {
    // Read registry data from account via lgs wallet
    let v: Value = serde_json::from_str(args).map_err(|e| format!("invalid JSON: {}", e))?;
    let wallet_home = v["wallet_home"].as_str().unwrap_or("");
    let registry_pda = v["registry_pda"].as_str().ok_or("missing registry_pda")?;

    let mut cmd = Command::new("lgs");
    cmd.arg("wallet")
        .arg("--")
        .arg("account")
        .arg("get")
        .arg("--account-id")
        .arg(format!("Public/{}", registry_pda))
        .arg("--raw");
    if !wallet_home.is_empty() {
        cmd.env("LEE_WALLET_HOME_DIR", wallet_home);
    }

    let output = cmd
        .output()
        .map_err(|e| format!("lgs wallet: {}", e))?;

    if !output.status.success() {
        let combined = format!(
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        if combined.contains("not found") || combined.contains("does not exist") {
            return Ok(json!({"ok": true, "entries": {}}).to_string());
        }
        return Err(format!("lgs wallet failed: {}", combined));
    }

    let stdout = String::from_utf8_lossy(&output.stdout);
    for line in stdout.lines() {
        let line = line.trim();
        if !line.starts_with('{') {
            continue;
        }
        if let Ok(obj) = serde_json::from_str::<Value>(line) {
            if let Some(hex_data) = obj["data"].as_str() {
                let bytes = hex::decode(hex_data)
                    .map_err(|e| format!("hex decode: {}", e))?;
                let reg = Registry::try_from_slice(&bytes)
                    .map_err(|e| format!("borsh decode: {}", e))?;

                let mut entries = serde_json::Map::new();
                for (cid, rec) in reg.entries {
                    entries.insert(
                        cid,
                        json!({
                            "metadata_hash":    hex::encode(rec.metadata_hash),
                            "anchor_timestamp": rec.anchor_timestamp,
                            "anchored_by":      hex::encode(rec.anchored_by),
                            "version":          rec.version,
                        }),
                    );
                }
                return Ok(json!({"ok": true, "entries": entries}).to_string());
            }
        }
    }

    Ok(json!({"ok": true, "entries": {}}).to_string())
}

// ─── Utility ─────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn chronicle_registry_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}

#[no_mangle]
pub extern "C" fn chronicle_registry_version() -> *mut c_char {
    to_cstring("0.1.0".to_string())
}
