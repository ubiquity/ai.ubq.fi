//! Named context codec: stream typed fields before serde's internal-tag buffer can lose RawValue.
use super::NativeRenewalContext;
use gateway_core::{identity::AccountBinding, model::ModelId};
use serde::{
    Deserialize, Deserializer,
    de::{Error, MapAccess, Visitor},
};
use std::fmt;
const ALL: &[&str] = &[
    "kind",
    "attempt_id",
    "original_admission_digest",
    "model",
    "slot",
    "binding",
    "active_generation",
    "model_utf16",
    "call_id",
    "mapping_revision",
    "context",
];
#[derive(Default)]
struct Fields {
    kind: Option<String>,
    attempt_id: Option<String>,
    original_admission_digest: Option<String>,
    model: Option<ModelId>,
    slot: Option<u64>,
    binding: Option<AccountBinding>,
    active_generation: Option<u64>,
    model_utf16: Option<Option<Vec<u16>>>,
    call_id: Option<String>,
    mapping_revision: Option<String>,
    context: Option<Box<gateway_cache_experiment_records::RefreshContext>>,
}
fn set<T, E: Error>(slot: &mut Option<T>, value: T, name: &'static str) -> Result<(), E> {
    if slot.replace(value).is_some() {
        return Err(E::duplicate_field(name));
    }
    Ok(())
}
fn take<T, E: Error>(slot: Option<T>, name: &'static str) -> Result<T, E> {
    slot.ok_or_else(|| E::missing_field(name))
}
impl Fields {
    fn reject_extra<E: Error>(&self, allowed: &'static [&'static str]) -> Result<(), E> {
        for (name, present) in [
            ("attempt_id", self.attempt_id.is_some()),
            (
                "original_admission_digest",
                self.original_admission_digest.is_some(),
            ),
            ("model", self.model.is_some()),
            ("slot", self.slot.is_some()),
            ("binding", self.binding.is_some()),
            ("active_generation", self.active_generation.is_some()),
            ("model_utf16", self.model_utf16.is_some()),
            ("call_id", self.call_id.is_some()),
            ("mapping_revision", self.mapping_revision.is_some()),
            ("context", self.context.is_some()),
        ] {
            if present && !allowed.contains(&name) {
                return Err(E::unknown_field(name, allowed));
            }
        }
        Ok(())
    }
    fn finish<E: Error>(mut self) -> Result<NativeRenewalContext, E> {
        let kind = take(self.kind.take(), "kind")?;
        match kind.as_str() {
            "inference" => {
                self.reject_extra::<E>(&[
                    "kind",
                    "attempt_id",
                    "original_admission_digest",
                    "model",
                    "slot",
                ])?;
                Ok(NativeRenewalContext::Inference {
                    attempt_id: take(self.attempt_id, "attempt_id")?,
                    original_admission_digest: take(
                        self.original_admission_digest,
                        "original_admission_digest",
                    )?,
                    model: take(self.model, "model")?,
                    slot: take(self.slot, "slot")?,
                })
            }
            "live_create" => {
                self.reject_extra::<E>(&["kind", "binding", "active_generation", "model_utf16"])?;
                Ok(NativeRenewalContext::LiveCreate {
                    binding: take(self.binding, "binding")?,
                    active_generation: take(self.active_generation, "active_generation")?,
                    model_utf16: take(self.model_utf16, "model_utf16")?,
                })
            }
            "live_mapped" => {
                self.reject_extra::<E>(&["kind", "call_id", "mapping_revision"])?;
                Ok(NativeRenewalContext::LiveMapped {
                    call_id: take(self.call_id, "call_id")?,
                    mapping_revision: take(self.mapping_revision, "mapping_revision")?,
                })
            }
            "cache_experiment" => {
                self.reject_extra::<E>(&["kind", "context"])?;
                Ok(NativeRenewalContext::CacheExperiment {
                    context: take(self.context, "context")?,
                })
            }
            _ => Err(E::unknown_variant(
                &kind,
                &[
                    "inference",
                    "live_create",
                    "live_mapped",
                    "cache_experiment",
                ],
            )),
        }
    }
}
impl<'de> Deserialize<'de> for NativeRenewalContext {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct ContextVisitor;
        impl<'de> Visitor<'de> for ContextVisitor {
            type Value = NativeRenewalContext;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a native renewal context object")
            }
            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
                let mut fields = Fields::default();
                while let Some(key) = map.next_key::<String>()? {
                    match key.as_str() {
                        "kind" => set(&mut fields.kind, map.next_value()?, "kind")?,
                        "attempt_id" => {
                            set(&mut fields.attempt_id, map.next_value()?, "attempt_id")?
                        }
                        "original_admission_digest" => set(
                            &mut fields.original_admission_digest,
                            map.next_value()?,
                            "original_admission_digest",
                        )?,
                        "model" => set(&mut fields.model, map.next_value()?, "model")?,
                        "slot" => set(&mut fields.slot, map.next_value()?, "slot")?,
                        "binding" => set(&mut fields.binding, map.next_value()?, "binding")?,
                        "active_generation" => set(
                            &mut fields.active_generation,
                            map.next_value()?,
                            "active_generation",
                        )?,
                        "model_utf16" => {
                            set(&mut fields.model_utf16, map.next_value()?, "model_utf16")?
                        }
                        "call_id" => set(&mut fields.call_id, map.next_value()?, "call_id")?,
                        "mapping_revision" => set(
                            &mut fields.mapping_revision,
                            map.next_value()?,
                            "mapping_revision",
                        )?,
                        "context" => set(&mut fields.context, map.next_value()?, "context")?,
                        _ => return Err(A::Error::unknown_field(&key, ALL)),
                    }
                }
                fields.finish()
            }
        }
        deserializer.deserialize_map(ContextVisitor)
    }
}
