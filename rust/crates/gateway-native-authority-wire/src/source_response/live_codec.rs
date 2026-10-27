use super::SourceNativeLiveAuthorityResponseV2 as Response;
use crate::{live::NativeLiveRetirement, source_fence::SourceNativeLiveFenceV2};
use gateway_authority_actor_records::versions::LIVE_RESPONSE_V2;
use serde::{
    Deserialize, Deserializer,
    de::{Error, MapAccess, Visitor},
};
use std::{fmt, marker::PhantomData};
const ALL: &[&str] = &[
    "operation",
    "v",
    "authorization_ref",
    "fence",
    "access",
    "outcome",
];
struct Fields<A> {
    operation: Option<String>,
    v: Option<u8>,
    authorization_ref: Option<String>,
    fence: Option<SourceNativeLiveFenceV2>,
    access: Option<A>,
    outcome: Option<NativeLiveRetirement>,
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
impl<A> Fields<A> {
    fn finish<E: Error>(self) -> Result<Response<A>, E> {
        let operation = take(self.operation, "operation")?;
        if take(self.v, "v")? != LIVE_RESPONSE_V2 {
            return Err(E::custom("unsupported source live response version"));
        }
        match operation.as_str() {
            "authorize" if self.outcome.is_none() => Ok(Response::Authorize {
                v: 2,
                authorization_ref: take(self.authorization_ref, "authorization_ref")?,
                fence: take(self.fence, "fence")?,
                access: take(self.access, "access")?,
            }),
            "recheck" if self.authorization_ref.is_none() && self.outcome.is_none() => {
                Ok(Response::Recheck {
                    v: 2,
                    fence: take(self.fence, "fence")?,
                    access: take(self.access, "access")?,
                })
            }
            "retire"
                if self.authorization_ref.is_none()
                    && self.fence.is_none()
                    && self.access.is_none() =>
            {
                Ok(Response::Retire {
                    v: 2,
                    outcome: take(self.outcome, "outcome")?,
                })
            }
            _ => Err(E::custom("invalid source live response fields")),
        }
    }
}
impl<'de, A: Deserialize<'de>> Deserialize<'de> for Response<A> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct LiveVisitor<A>(PhantomData<A>);
        impl<'de, A: Deserialize<'de>> Visitor<'de> for LiveVisitor<A> {
            type Value = Response<A>;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a source live response2 object")
            }
            fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<Self::Value, M::Error> {
                let mut fields = Fields {
                    operation: None,
                    v: None,
                    authorization_ref: None,
                    fence: None,
                    access: None,
                    outcome: None,
                };
                while let Some(key) = map.next_key::<String>()? {
                    match key.as_str() {
                        "operation" => set(&mut fields.operation, map.next_value()?, "operation")?,
                        "v" => set(&mut fields.v, map.next_value()?, "v")?,
                        "authorization_ref" => set(
                            &mut fields.authorization_ref,
                            map.next_value()?,
                            "authorization_ref",
                        )?,
                        "fence" => set(&mut fields.fence, map.next_value()?, "fence")?,
                        "access" => set(&mut fields.access, map.next_value()?, "access")?,
                        "outcome" => set(&mut fields.outcome, map.next_value()?, "outcome")?,
                        _ => return Err(M::Error::unknown_field(&key, ALL)),
                    }
                }
                fields.finish()
            }
        }
        deserializer.deserialize_map(LiveVisitor(PhantomData))
    }
}
