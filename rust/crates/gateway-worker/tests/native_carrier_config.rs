//! The one native carrier binding parses a fixed HTTPS transport identity and fails closed.
use gateway_auth::crypto::base64url;
use gateway_core::error::GatewayError;
use gateway_worker::native_host::{NATIVE_CARRIER_CONFIG_MAX_BYTES, NativeBindings};

type TestResult = Result<(), Box<dyn std::error::Error>>;
const INVALID: Option<&str> = Some("native_configuration_invalid");

fn identity(key: &str, capabilities: &str) -> String {
    format!(
        r#"{{"key_id":"native-key","owner_id":"native-owner","owner_epoch":"native-epoch","key_base64url":"{key}","capabilities":[{capabilities}]}}"#
    )
}

fn config(origin: &str, identity: &str) -> String {
    format!(r#"{{"origin":"{origin}","identity":{identity}}}"#)
}

fn message(error: GatewayError) -> String {
    error.envelope.error.message
}

fn error_code(raw: &str) -> Result<Option<String>, String> {
    match NativeBindings::from_binding_value(Some(raw)) {
        Ok(_) => Err(format!("invalid carrier configuration was accepted: {raw}")),
        Err(error) => Ok(error.envelope.error.code),
    }
}

#[test]
fn exact_binding_parses_a_fixed_https_transport_identity() -> TestResult {
    let key = base64url(&[7; 32]);
    let raw = config(
        "https://native.example",
        &identity(&key, r#""live_sideband","supervisor_read""#),
    );
    let bindings = NativeBindings::from_binding_value(Some(&raw))
        .map_err(message)?
        .ok_or("a configured binding")?;
    let identity = bindings.transport_identity().map_err(message)?;
    assert_eq!(identity.key_id(), "native-key");
    assert_eq!(identity.owner_id(), "native-owner");
    assert_eq!(identity.owner_epoch(), "native-epoch");
    Ok(())
}

#[test]
fn absent_binding_leaves_the_capability_unavailable() -> TestResult {
    assert!(
        NativeBindings::from_binding_value(None)
            .map_err(message)?
            .is_none()
    );
    Ok(())
}

#[test]
fn unknown_fields_and_capabilities_fail_closed() -> TestResult {
    let key = base64url(&[8; 32]);
    let unknown_capability = config(
        "https://native.example",
        &identity(&key, r#""live_sideband","future_capability""#),
    );
    let unknown_field = config(
        "https://native.example",
        &format!(
            r#"{{"key_id":"native-key","owner_id":"native-owner","owner_epoch":"native-epoch","key_base64url":"{key}","capabilities":["live_sideband"],"extra":true}}"#
        ),
    );
    for raw in [unknown_capability, unknown_field] {
        assert_eq!(error_code(&raw)?.as_deref(), INVALID);
    }
    Ok(())
}

#[test]
fn malformed_origins_and_keys_fail_closed() -> TestResult {
    let key = base64url(&[9; 32]);
    for origin in [
        "http://native.example",
        "https://native.example:8443",
        "https://user@native.example",
        "https://native.example/path",
        "https://native.example?target=elsewhere",
        "https://native.example#fragment",
    ] {
        let raw = config(origin, &identity(&key, r#""live_sideband""#));
        assert_eq!(error_code(&raw)?.as_deref(), INVALID);
    }
    let short = base64url(&[9; 31]);
    let raw = config(
        "https://native.example",
        &identity(&short, r#""live_sideband""#),
    );
    assert_eq!(error_code(&raw)?.as_deref(), INVALID);
    Ok(())
}

#[test]
fn empty_or_partial_identity_is_rejected() {
    let key = base64url(&[10; 32]);
    for raw in [
        config(
            "https://native.example",
            &identity(&key, r#""live_sideband""#),
        )
        .replace(r#","owner_id":"native-owner""#, ""),
        config("https://native.example", &identity(&key, "")),
        config("https://native.example", "{}"),
        String::new(),
    ] {
        assert!(NativeBindings::from_binding_value(Some(&raw)).is_err());
    }
}

#[test]
fn oversized_configuration_is_rejected_before_json_parsing() -> TestResult {
    let raw = "x".repeat(NATIVE_CARRIER_CONFIG_MAX_BYTES + 1);
    assert_eq!(error_code(&raw)?.as_deref(), INVALID);
    Ok(())
}

/// Moved from the in-source `NativeOrigin` tests: every alternate authority or non-root input
/// fails closed through the one binding parser.
#[test]
fn https_origin_rejects_alternate_authorities_and_nonroot_input() -> TestResult {
    let key = base64url(&[11; 32]);
    for origin in [
        "http://native.example",
        "https:native.example",
        "https:///native.example",
        "https://native.example:8443",
        "https://user@native.example",
        "https://@native.example",
        "https://user:password@native.example",
        "https://native.example/path",
        "https://native.example/../",
        "https://native.example/%2e/",
        "https://native.example//",
        "https://native.example?target=elsewhere",
        "https://native.example?",
        "https://native.example#fragment",
        "https://native.example#",
        "https://native.example\\elsewhere",
        "https://native.example\n",
        " https://native.example",
        "",
    ] {
        let raw = format!(
            r#"{{"origin":{},"identity":{}}}"#,
            serde_json::to_string(origin)?,
            identity(&key, r#""live_sideband""#)
        );
        assert_eq!(error_code(&raw)?.as_deref(), INVALID, "accepted {origin:?}");
    }
    for origin in ["https://native.example/", "https://native.example:443/"] {
        let raw = config(origin, &identity(&key, r#""live_sideband""#));
        assert!(
            NativeBindings::from_binding_value(Some(&raw)).is_ok(),
            "rejected {origin:?}"
        );
    }
    Ok(())
}

const RENEWAL: &str = r#""live_sideband","owner_renewal","access_publish","supervisor_read""#;

fn with_owners(key: &str, capabilities: &str, owners: &str) -> String {
    format!(
        r#"{{"origin":"https://native.example","identity":{},"renewal_owners":[{owners}]}}"#,
        identity(key, capabilities)
    )
}

#[test]
fn renewal_owner_registry_configures_fixed_codex_custody() -> TestResult {
    let key = base64url(&[12; 32]);
    let one = with_owners(
        &key,
        RENEWAL,
        r#"{"owner_id":"vps-a","codex_home":"/home/codex/.codex"}"#,
    );
    let two = with_owners(
        &key,
        RENEWAL,
        r#"{"owner_id":"vps-a","codex_home":"/home/codex/.codex"},{"owner_id":"vps-b","codex_home":"/home/codex/.codex-b"}"#,
    );
    for raw in [one, two] {
        let bindings = NativeBindings::from_binding_value(Some(&raw))
            .map_err(message)?
            .ok_or("a configured binding")?;
        assert!(bindings.renewal_owners().is_ok());
    }
    // The pre-existing shape remains valid and still grants no renewal ownership.
    let plain = config(
        "https://native.example",
        &identity(&key, r#""live_sideband""#),
    );
    let bindings = NativeBindings::from_binding_value(Some(&plain))
        .map_err(message)?
        .ok_or("a configured binding")?;
    assert!(bindings.renewal_owners().is_err());
    Ok(())
}

#[test]
fn renewal_registry_and_capabilities_must_pair_exactly() -> TestResult {
    let key = base64url(&[13; 32]);
    let owner = r#"{"owner_id":"vps-a","codex_home":"/home/codex/.codex"}"#;
    for raw in [
        // A registry without both renewal capabilities.
        with_owners(&key, r#""live_sideband""#, owner),
        with_owners(&key, r#""owner_renewal""#, owner),
        with_owners(&key, r#""access_publish""#, owner),
        // Renewal capabilities without a registry would silently disable refresh.
        config(
            "https://native.example",
            &identity(&key, r#""owner_renewal","access_publish""#),
        ),
        config(
            "https://native.example",
            &identity(&key, r#""owner_renewal""#),
        ),
        with_owners(&key, RENEWAL, ""),
    ] {
        assert_eq!(error_code(&raw)?.as_deref(), INVALID, "accepted {raw}");
    }
    Ok(())
}

#[test]
fn renewal_registry_entries_fail_closed() -> TestResult {
    let key = base64url(&[14; 32]);
    let a = r#"{"owner_id":"vps-a","codex_home":"/home/codex/.codex"}"#;
    for owners in [
        r#"{"owner_id":"vps-a","codex_home":"relative/.codex"}"#.to_owned(),
        r#"{"owner_id":"","codex_home":"/home/codex/.codex"}"#.to_owned(),
        r#"{"owner_id":"vps-a","codex_home":"/home/codex/.codex","extra":1}"#.to_owned(),
        r#"{"owner_id":"vps-a"}"#.to_owned(),
        // A duplicate owner id or a duplicate home.
        format!(r#"{a},{{"owner_id":"vps-a","codex_home":"/home/codex/.codex-b"}}"#),
        format!(r#"{a},{{"owner_id":"vps-b","codex_home":"/home/codex/.codex"}}"#),
        // More than the two independent owners the authority registry admits.
        format!(
            r#"{a},{{"owner_id":"vps-b","codex_home":"/b"}},{{"owner_id":"vps-c","codex_home":"/c"}}"#
        ),
    ] {
        let raw = with_owners(&key, RENEWAL, &owners);
        assert_eq!(error_code(&raw)?.as_deref(), INVALID, "accepted {owners}");
    }
    Ok(())
}
