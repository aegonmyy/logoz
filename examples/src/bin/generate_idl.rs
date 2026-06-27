/// Generate IDL JSON for the chronicle-registry program.
///
/// Usage:
///   cargo run --bin generate_idl > chronicle-registry-idl.json

spel_framework::generate_idl!("../methods/guest/src/bin/chronicle_registry.rs");
