//! Read each fence from the original deserializer, never Serde's buffered Content.
//! Its source-preserving context decoder needs serde_json's raw token support.
use super::SourceNativeRenewalAuthorityResponseV2 as Response;
use crate::{renewal::NativeRenewalMode, source_fence::SourceNativeRenewalFenceV3};
use gateway_authority_actor_records::versions::AUTHORITY_RESPONSE_V2;
use serde::{
    Deserialize, Deserializer,
    de::{Error, MapAccess, Visitor},
};
use std::fmt;

const ALL: &[&str] = &[
    "operation",
    "v",
    "action_ref",
    "operation_id",
    "claim_ref",
    "start_permit",
    "publication_revision",
    "mode",
    "deadline_ms",
    "fence",
];
#[derive(Default)]
struct Fields {
    operation: Option<String>,
    v: Option<u8>,
    action_ref: Option<String>,
    operation_id: Option<String>,
    claim_ref: Option<String>,
    start_permit: Option<Option<String>>,
    publication_revision: Option<Option<String>>,
    mode: Option<NativeRenewalMode>,
    deadline_ms: Option<i64>,
    fence: Option<SourceNativeRenewalFenceV3>,
}
fn set<T, E: Error>(slot: &mut Option<T>, value: T, name: &'static str) -> Result<(), E> {
    if slot.replace(value).is_some() {
        return Err(E::duplicate_field(name));
    }
    Ok(())
}
fn take<T, E: Error>(value: Option<T>, name: &'static str) -> Result<T, E> {
    value.ok_or_else(|| E::missing_field(name))
}
impl Fields {
    fn reject_extra<E: Error>(&self, allowed: &'static [&'static str]) -> Result<(), E> {
        for (key, present) in [
            ("action_ref", self.action_ref.is_some()),
            ("operation_id", self.operation_id.is_some()),
            ("claim_ref", self.claim_ref.is_some()),
            ("start_permit", self.start_permit.is_some()),
            ("publication_revision", self.publication_revision.is_some()),
            ("mode", self.mode.is_some()),
            ("deadline_ms", self.deadline_ms.is_some()),
            ("fence", self.fence.is_some()),
        ] {
            if present && !allowed.contains(&key) {
                return Err(E::unknown_field(key, allowed));
            }
        }
        Ok(())
    }
    fn finish<E: Error>(mut self) -> Result<Response, E> {
        let operation = take(self.operation.take(), "operation")?;
        let v = take(self.v.take(), "v")?;
        if v != AUTHORITY_RESPONSE_V2 {
            return Err(E::custom("unsupported source renewal response version"));
        }
        match operation.as_str() {
            "claim" => {
                self.reject_extra::<E>(ALL)?;
                Ok(Response::Claim {
                    v,
                    action_ref: take(self.action_ref, "action_ref")?,
                    operation_id: take(self.operation_id, "operation_id")?,
                    claim_ref: take(self.claim_ref, "claim_ref")?,
                    start_permit: self.start_permit.flatten(),
                    publication_revision: self.publication_revision.flatten(),
                    mode: take(self.mode, "mode")?,
                    deadline_ms: take(self.deadline_ms, "deadline_ms")?,
                    fence: take(self.fence, "fence")?,
                })
            }
            "start" => {
                self.reject_extra::<E>(&["mode"])?;
                Ok(Response::Start {
                    v,
                    mode: take(self.mode, "mode")?,
                })
            }
            "recheck" => {
                self.reject_extra::<E>(&["fence"])?;
                Ok(Response::Recheck {
                    v,
                    fence: take(self.fence, "fence")?,
                })
            }
            "published" => {
                self.reject_extra::<E>(&["publication_revision"])?;
                Ok(Response::Published {
                    v,
                    publication_revision: take(
                        self.publication_revision.flatten(),
                        "publication_revision",
                    )?,
                })
            }
            "unknown" => {
                self.reject_extra::<E>(&[])?;
                Ok(Response::Unknown { v })
            }
            _ => Err(E::unknown_variant(
                &operation,
                &["claim", "start", "recheck", "published", "unknown"],
            )),
        }
    }
}
impl<'de> Deserialize<'de> for Response {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct ResponseVisitor;
        impl<'de> Visitor<'de> for ResponseVisitor {
            type Value = Response;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a fixed native renewal authority response object")
            }
            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Response, A::Error> {
                let mut fields = Fields::default();
                while let Some(key) = map.next_key::<String>()? {
                    match key.as_str() {
                        "operation" => set(&mut fields.operation, map.next_value()?, "operation")?,
                        "v" => set(&mut fields.v, map.next_value()?, "v")?,
                        "action_ref" => {
                            set(&mut fields.action_ref, map.next_value()?, "action_ref")?
                        }
                        "operation_id" => {
                            set(&mut fields.operation_id, map.next_value()?, "operation_id")?
                        }
                        "claim_ref" => set(&mut fields.claim_ref, map.next_value()?, "claim_ref")?,
                        "start_permit" => {
                            set(&mut fields.start_permit, map.next_value()?, "start_permit")?
                        }
                        "publication_revision" => set(
                            &mut fields.publication_revision,
                            map.next_value()?,
                            "publication_revision",
                        )?,
                        "mode" => set(&mut fields.mode, map.next_value()?, "mode")?,
                        "deadline_ms" => {
                            set(&mut fields.deadline_ms, map.next_value()?, "deadline_ms")?
                        }
                        "fence" => set(&mut fields.fence, map.next_value()?, "fence")?,
                        _ => return Err(A::Error::unknown_field(&key, ALL)),
                    }
                }
                fields.finish()
            }
        }
        deserializer.deserialize_map(ResponseVisitor)
    }
}
