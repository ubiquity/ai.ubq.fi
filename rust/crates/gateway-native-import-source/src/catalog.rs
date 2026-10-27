//! Source import parsing/catalog/error projections only. DATA never authenticates a candidate or grants an effect.
use crate::{documents as document, error::unavailable};
use gateway_auth_credentials::native_credentials::NativeCredentials;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};

pub enum CatalogValidation {
    Normalized {
        snapshot: SourceJson,
        client_version: String,
        upstream_status: u16,
    },
    AuthenticatedLimited,
}

pub fn tokens_replaced(
    tokens: &SourceJson,
    rotated: &SourceJson,
    previous: &NativeCredentials,
) -> Result<Vec<(JsString16, SourceJson)>, GatewayError> {
    let mut fields = tokens.object_entries()?;
    fields.retain(|(k, _)| {
        !["access_token", "refresh_token"]
            .iter()
            .any(|n| *k == JsString16::from_utf8(n))
    });
    let access = document::member(rotated, "access_token")?
        .filter(|v| nonempty_string(v).is_some())
        .ok_or_else(unavailable)?;
    let refresh = document::member(rotated, "refresh_token")?
        .filter(|v| nonempty_string(v).is_some())
        .unwrap_or(SourceJson::from_serializable(
            previous.refresh_token.expose(),
        )?);
    fields.push((JsString16::from_utf8("access_token"), access));
    fields.push((JsString16::from_utf8("refresh_token"), refresh));
    Ok(fields)
}
fn nonempty_string(value: &SourceJson) -> Option<String> {
    let text: String = value.deserialize().ok()?;
    (!text.trim().is_empty()).then_some(text)
}
pub fn selected_version(version: Option<&JsString16>) -> String {
    let supplied = version.and_then(|v| v.try_utf8().ok()).map(|v| {
        v.trim_matches(|c: char| c.is_whitespace() || c == '\u{feff}')
            .to_owned()
    });
    let valid = supplied.as_deref().is_some_and(|v| {
        let parts: Vec<_> = v.split('.').collect();
        parts.len() == 3
            && parts.iter().all(|p| {
                !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) && {
                    let digits = p.trim_start_matches('0');
                    digits.is_empty()
                        || (digits.len() <= 16
                            && digits.parse::<u64>().is_ok_and(|n| {
                                n <= gateway_core::identity::MAX_SAFE_INTEGER as u64
                            }))
                }
            })
    });
    if valid {
        supplied.unwrap_or_else(|| "0.100.0".into())
    } else {
        "0.100.0".into()
    }
}
pub fn normalized_catalog(
    body: &SourceJson,
    version: &str,
    status: u16,
    now: i64,
) -> Result<CatalogValidation, GatewayError> {
    // This is the actual shared source normalizer, not a second native interpretation.
    let prepared =
        gateway_authority::native_control::import::publication_wire::prepare_import_catalog(
            body, version, status, now,
        )
        .map_err(|error| {
            if error.status == 503 {
                invalid_catalog()
            } else {
                error
            }
        })?;
    let gateway_auth_native_wire::publication_command::ImportCatalogData::Normalized {
        snapshot,
        client_version,
        upstream_status,
    } = prepared
    else {
        return Err(invalid_catalog());
    };
    Ok(CatalogValidation::Normalized {
        snapshot,
        client_version,
        upstream_status,
    })
}
pub fn invalid_catalog() -> GatewayError {
    GatewayError::new(
        502,
        "Codex upstream models response did not include a non-empty model catalog",
        Some("codex_upstream_unreachable"),
    )
}
pub fn model_error(status: u16, body: &[u8], credentials: &NativeCredentials) -> GatewayError {
    let mut text = String::from_utf8_lossy(body).into_owned();
    for secret in [
        credentials.access_token.expose(),
        credentials.refresh_token.expose(),
    ] {
        if !secret.is_empty() {
            text = text.replace(secret, "[redacted]");
        }
    }
    let text = JsString16::from_utf8(&text)
        .prefix_units(8000)
        .try_utf8()
        .unwrap_or_default();
    GatewayError::new(
        401,
        format!("Invalid Codex auth.json (upstream {}): {text}", status),
        Some("invalid_api_key"),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_shared_catalog_normalization_preserves_source_empty_and_safe_size_errors() {
        let empty = SourceJson::parse(r#"{"models":[]}"#).unwrap();
        let error = match normalized_catalog(&empty, "0.100.0", 200, 1) {
            Err(e) => e,
            Ok(_) => panic!("empty catalog accepted"),
        };
        assert_eq!(error.status, 502);
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("codex_upstream_unreachable")
        );
        let input = SourceJson::parse(
            r#"{"models":[{"slug":"synthetic_model","display_name":"Synthetic"}]}"#,
        )
        .unwrap();
        assert!(normalized_catalog(&input, "0.100.0", 200, 1).is_ok());
        let oversized_version = format!("{}.0.0", "0".repeat(60_000));
        let error = match normalized_catalog(&input, &oversized_version, 200, 1) {
            Err(e) => e,
            Ok(_) => panic!("oversized source snapshot accepted"),
        };
        assert_eq!(error.status, 413);
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("invalid_request_error")
        );
    }
    #[test]
    fn source_client_version_precedence_grammar_and_safe_integer_domain_are_not_narrowed() {
        assert_eq!(selected_version(None), "0.100.0");
        for invalid in [
            "",
            "1.2",
            "-1.2.3",
            "1.2.3-beta",
            "9007199254740992.0.0",
            "NaN.0.0",
        ] {
            assert_eq!(
                selected_version(Some(&JsString16::from_utf8(invalid))),
                "0.100.0"
            );
        }
        for valid in ["1.2.3", "01.002.0003", "9007199254740991.0.0"] {
            assert_eq!(selected_version(Some(&JsString16::from_utf8(valid))), valid);
        }
        let zeros = format!("{}.0.0", "0".repeat(3000));
        assert_eq!(
            selected_version(Some(&JsString16::from_utf8(&zeros))),
            zeros
        );
        assert_eq!(
            selected_version(Some(&JsString16::from_utf8("\u{feff} 1.2.3 \u{feff}"))),
            "1.2.3"
        );
        assert_eq!(
            selected_version(Some(&JsString16::from_units(vec![0xd800]))),
            "0.100.0"
        );
    }
}
