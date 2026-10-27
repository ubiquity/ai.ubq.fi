use gateway_provider_source::source as s;
use gateway_core::error::GatewayError;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as source;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema::source_request::value::Value;
use crate::extras::image::ImagePlan;
use crate::extras::image::ParsedImageRequest;
use serde_json::Value as ScalarValue;
use gateway_provider_source::extras::scalar_error;
use gateway_provider_source::extras::lift;

pub mod image;


#[derive(Clone, Debug)]
pub struct MultipartFile {
    pub name: String,
    pub filename: String,
    pub content_type: String,
    pub bytes: Vec<u8>,
}


pub fn parse_source_image(
    raw: &SourceJson,
    edit: bool,
) -> Result<ParsedImageRequest, SourceRequestError> {
    image::parse_image_view(&Value::from_source(raw.clone()), edit)
}


pub fn parse_source_multipart_image(
    raw: &SourceJson,
    files: &[MultipartFile],
) -> Result<ParsedImageRequest, SourceRequestError> {
    image::parse_multipart_image_view(&Value::from_source(raw.clone()), files)
}


pub fn parse_image(raw: &ScalarValue, edit: bool) -> Result<ParsedImageRequest, GatewayError> {
    parse_source_image(&lift(raw)?, edit).map_err(s::scalar_error)
}


pub fn parse_multipart_image(
    raw: &ScalarValue,
    files: &[MultipartFile],
) -> Result<ParsedImageRequest, GatewayError> {
    parse_source_multipart_image(&lift(raw)?, files).map_err(s::scalar_error)
}


pub fn prepare_image(
    raw: &ScalarValue,
    model: &str,
    edit: bool,
    id: &str,
) -> Result<ImagePlan, GatewayError> {
    image::prepare_image_view(&Value::from_source(lift(raw)?), &model.into(), edit, id)
        .map_err(s::scalar_error)
}


pub fn prepare_multipart_image(
    raw: &ScalarValue,
    files: &[MultipartFile],
    model: &str,
    id: &str,
) -> Result<ImagePlan, GatewayError> {
    image::prepare_multipart_image_view(&Value::from_source(lift(raw)?), files, &model.into(), id)
        .map_err(s::scalar_error)
}


pub fn normalize_image_reference(
    raw: &ScalarValue,
    mask: bool,
) -> Result<ScalarValue, GatewayError> {
    let value = image::normalize_image_reference_view(&Value::from_source(lift(raw)?), mask)
        .map_err(s::scalar_error)?;
    value
        .source()
        .and_then(|raw| raw.deserialize())
        .map_err(|_| {
            GatewayError::new(
                400,
                "Invalid scalar image reference",
                Some("invalid_request_error"),
            )
        })
}


pub fn multipart_image_references(
    files: &[MultipartFile],
) -> Result<(Vec<ScalarValue>, Option<ScalarValue>), GatewayError> {
    let (images, mask) =
        image::multipart_image_references(files).map_err(s::scalar_error)?;
    let images = images.iter().map(|image| image.source()?.deserialize()).collect::<Result<Vec<ScalarValue>, GatewayError>>()?;
    let mask = match mask { Some(mask) => Some(mask.source()?.deserialize()?), None => None };
    Ok((images, mask))
}


pub fn extract_source_images(raw: &SourceJson) -> Result<Vec<SourceJson>, GatewayError> {
    let mut images = Vec::new();
    for value in source::member(raw, "output").and_then(|value| source::array(&value)).into_iter().flatten() {
        if !source::member(&value, "type").is_some_and(|kind| s::is(&kind, "image_generation_call")) { continue; }
        let Some(result) = source::member(&value, "result").and_then(|value| source::string(&value)).filter(|text| !text.as_units().is_empty()) else { continue; };
        let mut image = s::object(vec![("b64_json", s::quoted(&result)?)])?;
        if let Some(prompt) = source::member(&value, "revised_prompt").and_then(|value| source::string(&value)) { s::set(&mut image, "revised_prompt", s::quoted(&prompt)?)?; }
        images.push(image);
    }
    Ok(images)
}

pub fn extract_images(payload: &ScalarValue) -> Result<Vec<ScalarValue>, GatewayError> {
    extract_source_images(&lift(payload)?)?.into_iter().map(|image| image.deserialize()).collect()
}
