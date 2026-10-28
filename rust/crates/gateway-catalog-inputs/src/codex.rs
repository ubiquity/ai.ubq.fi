//! Fixed Codex catalog reads and source evidence DATA; no lease or publication authority.
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogObservation, CodexAccount,
    CredentialPublication, unavailable,
};
use gateway_contracts::model::ModelId;
use gateway_schema::{catalog_cache, snapshot::union_account_catalogs};
use gateway_schema_values::source_view::js_trim_source;
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
/// Observed catalog DATA only; no public write operation accepts this as permission.
pub struct Fetched {
    pub response: CatalogFetchResponse,
    pub evidence: Vec<(String, Vec<ModelId>)>,
    pub publication: CredentialPublication,
    pub degraded: bool,
}
async fn fetch_account<H: CatalogHost>(
    host: &H,
    account: &CodexAccount,
    version: &str,
    etag: Option<String>,
) -> Result<(CatalogFetchResponse, CodexAccount), GatewayError> {
    let mut account = account.clone();
    let mut request = CatalogFetch::Codex {
        account: account.clone(),
        client_version: version.into(),
        if_none_match: etag.clone(),
    };
    let mut response = observed_fetch(host, request.clone(), &account).await?;
    if response.status == 401 {
        account = host.refresh_codex(&account).await?;
        let latest = host.publication().await?;
        if !latest.codex_accounts.iter().any(|a| a == &account) {
            return Err(unavailable(
                "The credential owner did not durably publish its refreshed identity",
            ));
        }
        request = CatalogFetch::Codex {
            account: account.clone(),
            client_version: version.into(),
            if_none_match: etag,
        };
        response = observed_fetch(host, request.clone(), &account).await?;
    }
    if request
        .max_body_bytes()
        .is_some_and(|limit| response.body.len() > limit)
    {
        return Err(unavailable(
            "Codex model catalog exceeded its response bound",
        ));
    }
    Ok((response, account))
}
async fn observed_fetch<H: CatalogHost>(
    host: &H,
    request: CatalogFetch,
    account: &CodexAccount,
) -> Result<CatalogFetchResponse, GatewayError> {
    match host.fetch(request).await {
        Ok(response) => {
            let _ = host
                .observe(CatalogObservation::CodexResponse {
                    account: account.clone(),
                    status: response.status,
                    provider_request_id: response
                        .provider_request_id
                        .as_deref()
                        .and_then(normalized_request_id),
                    access_token_expired: response.access_token_expired,
                    observed_at_ms: response.observed_at_ms,
                })
                .await;
            Ok(response)
        }
        Err(error) => {
            let _ = host
                .observe(CatalogObservation::CodexTransportFailure {
                    account: account.clone(),
                    code: error.envelope.error.code.clone(),
                })
                .await;
            Err(error)
        }
    }
}
fn normalized_request_id(raw: &str) -> Option<String> {
    let value = gateway_schema_values::scalar::js_trim(raw);
    (!value.is_empty()
        && value.encode_utf16().count() <= 256
        && !value.chars().any(|c| (c as u32) < 0x20 || c == '\u{7f}'))
    .then(|| value.to_owned())
}
pub fn degraded(error: &GatewayError) -> bool {
    matches!(
        error.envelope.error.code.as_deref(),
        Some("gateway_timeout" | "codex_upstream_unreachable")
    )
}
fn evidence(account: &str, parsed: &SourceJson) -> Vec<(String, Vec<ModelId>)> {
    let Some(rows) = source::member(parsed, "models")
        .as_ref()
        .and_then(source::array)
    else {
        return Vec::new();
    };
    let mut slugs = Vec::new();
    for row in rows {
        let slug = ["slug", "id", "model", "name"]
            .iter()
            .find_map(|key| source::member(&row, key).as_ref().and_then(source::string));
        if let Some(slug) = slug
            .map(|s| js_trim_source(&s))
            .filter(|s| !s.as_units().is_empty())
        {
            let slug = ModelId::from_source(slug);
            if !slugs.contains(&slug) {
                slugs.push(slug);
            }
        }
    }
    vec![(account.into(), slugs)]
}
pub async fn fetch_codex<H: CatalogHost>(
    host: &H,
    initial: &CredentialPublication,
    version: &str,
    etag: Option<String>,
) -> Result<Fetched, GatewayError> {
    if initial.codex_accounts.is_empty() {
        return Err(unavailable("Codex authentication is unavailable"));
    }
    let union = initial.codex_accounts.len() > 1;
    let mut catalogs = Vec::new();
    let mut fallback = None;
    let mut last_error = None;
    let mut accepted = initial.clone();
    let mut degraded = false;
    for account in &initial.codex_accounts {
        let (response, used) = match fetch_account(
            host,
            account,
            version,
            if union { None } else { etag.clone() },
        )
        .await
        {
            Ok(result) => result,
            Err(error) => {
                degraded |= self::degraded(&error);
                last_error = Some(error);
                continue;
            }
        };
        degraded |= (500..600).contains(&response.status);
        // An owned renewal can change the exact pool revision; no uncommitted return is accepted.
        let latest = host.publication().await?;
        let expected_accounts = accepted
            .codex_accounts
            .iter_mut()
            .find(|a| a.account_id == used.account_id);
        if let Some(expected) = expected_accounts {
            *expected = used.clone();
        }
        if latest.codex_accounts != accepted.codex_accounts {
            return Err(catalog_cache::rotation_unavailable());
        }
        accepted = latest;
        if !union {
            let evidence = if (200..300).contains(&response.status)
                && response.single_account_observation_complete
                && response.body.len() <= 4 * 1024 * 1024
            {
                SourceJson::parse(&response.body)
                    .ok()
                    .map(|p| evidence(&used.account_id, &p))
                    .unwrap_or_default()
            } else {
                Vec::new()
            };
            return Ok(Fetched {
                response,
                evidence,
                publication: accepted,
                degraded,
            });
        }
        if (200..300).contains(&response.status)
            && let Ok(parsed) = SourceJson::parse(&response.body)
            && source::entries(&parsed).is_some()
            && source::member(&parsed, "models")
                .as_ref()
                .and_then(source::array)
                .is_some()
        {
            catalogs.push((used.account_id, parsed));
            continue;
        }
        fallback = Some(response);
    }
    if let Some((parsed, evidence)) = union_account_catalogs(&catalogs) {
        return Ok(Fetched {
            response: CatalogFetchResponse::json(
                200,
                source::canonical(&parsed)?.as_str().to_owned(),
            ),
            evidence,
            publication: accepted,
            degraded,
        });
    }
    if let Some(response) = fallback {
        return Ok(Fetched {
            response,
            evidence: Vec::new(),
            publication: accepted,
            degraded,
        });
    }
    Err(last_error.unwrap_or_else(|| unavailable("Codex model discovery is unavailable")))
}
#[cfg(test)]
mod source_evidence_tests {
    use super::*;
    #[test]
    fn single_account_evidence_retains_trimmed_units_and_distinguishes_escape_text() {
        let document = SourceJson::parse(r#"{"models":[{"slug":" \ud800 "},{"slug":"\ud800"},{"slug":"\\ud800"},{"id":"\udc00"},{"slug":" "}]}"#).unwrap();
        let result = evidence("source-account", &document);
        assert_eq!(result[0].0, "source-account");
        assert_eq!(result[0].1.len(), 3);
        assert_eq!(result[0].1[0].as_js_string().as_units(), &[0xd800]);
        assert!(result[0].1[1].matches_scalar(r"\ud800"));
        assert_eq!(result[0].1[2].as_js_string().as_units(), &[0xdc00]);
    }
}
