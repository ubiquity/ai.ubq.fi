//! Internal source-string semantics; these do not claim ordinary JSON ingress supports lone surrogates.
use super::*;
type TestResult = Result<(), Box<dyn std::error::Error>>;

fn golden() -> serde_json::Result<Value> {
    serde_json::from_str(include_str!(
        "../../tests/fixtures/forwarding-unicode-source-v1.json"
    ))
}

#[test]
fn source_lone_units_are_not_replaced_when_already_in_budget() -> TestResult {
    let fixture = golden()?;
    let units = serde_json::from_value::<Vec<u16>>(fixture["inBudgetLoneUnits"].clone())
        .map_err(|error| format!("{error:?}"))?;
    let input = JsString16::from_units(units);
    let mut elisions = vec![];
    let actual = bound_source(&input, "input", None, "message", false, &mut elisions)
        .map_err(|error| format!("{error:?}"))?;
    assert_eq!(actual, input);
    assert!(elisions.is_empty());
    assert!(actual.try_utf8().is_err());
    let roundtrip: JsString16 = serde_json::from_str(actual.to_source_json()?.as_str())?;
    assert_eq!(roundtrip, input);
    Ok(())
}

#[test]
fn source_lone_units_count_as_textencoder_replacements_without_changing_kept_units() -> TestResult {
    let golden = golden()?;
    for (index, first) in [0xd800, 0xdc00].into_iter().enumerate() {
        let mut units = vec![first];
        units.extend(std::iter::repeat_n(97, FORWARD_LIMIT + 1024));
        let input = JsString16::from_units(units);
        let case = &golden["lone"][index];
        let head = serde_json::from_value::<usize>(case["headUnits"].clone())?;
        let marker = case["marker"]
            .as_str()
            .ok_or("missing golden forwarding marker")?;
        let mut expected = input.as_units()[..head].to_vec();
        expected.extend(marker.encode_utf16());
        let mut elisions = vec![];
        let actual = bound_source(
            &input,
            "input[1].output",
            Some(&JsString16::from_utf8("call_unicode")),
            "tool_output",
            false,
            &mut elisions,
        )
        .map_err(|error| format!("{error:?}"))?;
        assert_eq!(actual.as_units(), expected);
        assert_eq!(actual.as_units()[0], first);
        for (observed, field) in [
            (elisions[0].original_bytes, "originalBytes"),
            (elisions[0].forwarded_bytes, "forwardedBytes"),
            (elisions[0].omitted_bytes, "omittedBytes"),
        ] {
            let expected = serde_json::from_value::<usize>(case["elision"][field].clone())?;
            assert_eq!(observed, expected, "{field}");
        }
    }
    Ok(())
}
