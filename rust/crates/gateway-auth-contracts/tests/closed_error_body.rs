use gateway_auth_contracts::types::{AuthResponse, AuthResponseBody};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_source::{js_string::JsString16, source_json::SourceJson};

#[test]
fn closed_auth_response_keeps_defaults_utf16_and_original_status()
-> Result<(), Box<dyn std::error::Error>> {
    let error = SourceRequestError::new(
        422,
        JsString16::from_units(vec![0xd800, 0, 0xdc00]),
        " vendor ",
        Some("source_code"),
        Some(None),
    );
    let expected = error.body().as_bytes().to_vec();
    let mut response = AuthResponse::closed_error(error.status, error.body().clone());
    assert_eq!(response.status, 422);
    assert_eq!(response.body.to_bytes()?, expected);
    assert!(response.body.as_json().is_none());
    assert_eq!(response.headers["content-type"], "application/json");
    assert_eq!(response.headers["cache-control"], "no-store");
    assert!(response.stream.is_none());
    assert!(response.set_cookies.is_empty());
    response.set_cookies.push("a=one; HttpOnly".to_owned());
    response.set_cookies.push("b=two; HttpOnly".to_owned());
    response
        .headers
        .insert("content-type".to_owned(), "application/custom".to_owned());
    assert_eq!(response.set_cookies, ["a=one; HttpOnly", "b=two; HttpOnly"]);
    assert_eq!(response.headers["content-type"], "application/custom");
    assert_eq!(response.body.to_bytes()?, expected);
    Ok(())
}

#[test]
fn closed_auth_generic_json_and_direct_wire_views_match() -> Result<(), Box<dyn std::error::Error>>
{
    for param in [
        None,
        Some(None),
        Some(Some(JsString16::from_units(vec![0xdfff]))),
    ] {
        let error = SourceRequestError::new(400, "m\0\"", " t ", Some(""), param);
        let response = AuthResponse::closed_error(error.status, error.body().clone());
        assert_eq!(
            serde_json::to_vec(&response.body)?,
            response.body.to_bytes()?
        );
        let wire = SourceJson::parse_bytes(&response.body.to_bytes()?)?;
        assert_eq!(wire.as_bytes(), error.body().as_bytes());
    }
    Ok(())
}

#[test]
fn existing_auth_source_json_and_empty_body_contracts_stay_distinct()
-> Result<(), Box<dyn std::error::Error>> {
    let original = SourceJson::parse(r#"{"vendor":{"text":"\ud800"},"x":1,"x":2}"#)?;
    let response = AuthResponse::source_json(409, original.clone());
    assert_eq!(response.body.to_bytes()?, original.as_bytes());
    assert_eq!(serde_json::to_vec(&response.body)?, original.as_bytes());
    assert!(response.body.as_json().is_none());
    assert!(AuthResponseBody::Empty.to_bytes()?.is_empty());
    assert_eq!(serde_json::to_vec(&AuthResponseBody::Empty)?, b"null");
    Ok(())
}
