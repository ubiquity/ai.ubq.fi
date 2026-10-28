use crate::access_publication::{
    AccessCredentialSource, PreparedAccessPublication, RegisteredAccessOwner,
};
use crate::wire::{ConfigWire, PublicationDisposition, PublicationReceipt, WireCommand};
use gateway_auth_credentials::{
    native_credentials::NativeCredentials, native_credentials::NativeOwnerId,
};
use gateway_auth_native_proof::{
    native_auth::AuthenticatedNativeOwner, native_auth::NativeCapability,
    native_auth::NativeTransportIdentity,
};
use gateway_core::state::AuthorityStore;
use gateway_publication_policy::control_publication::{
    ConfiguredPaidInputs, PreparedConfigurationPublication, conflict,
};
use gateway_publication_policy::protocol::invalid;
use gateway_source::error::GatewayError;

struct ApplyContext<'a> {
    identity: &'a NativeTransportIdentity,
    inputs: &'a ConfiguredPaidInputs,
    registered: &'a [RegisteredAccessOwner],
    clock: fn() -> i64,
}

pub async fn apply_publication<S: AuthorityStore>(
    store: &S,
    proof: &AuthenticatedNativeOwner,
    body: &[u8],
    identity: &NativeTransportIdentity,
    inputs: &ConfiguredPaidInputs,
    registered: &[RegisteredAccessOwner],
    clock: fn() -> i64,
) -> Result<PublicationReceipt, GatewayError> {
    let context = ApplyContext {
        identity,
        inputs,
        registered,
        clock,
    };
    match WireCommand::parse(body)? {
        WireCommand::ConfigPublish(w) => apply_configuration(store, proof, body, &context, w).await,
        WireCommand::AccessPublish(w) => apply_access(store, proof, body, &context, w).await,
    }
}

async fn apply_configuration<S: AuthorityStore>(
    store: &S,
    proof: &AuthenticatedNativeOwner,
    body: &[u8],
    context: &ApplyContext<'_>,
    w: ConfigWire,
) -> Result<PublicationReceipt, GatewayError> {
    proof.require(NativeCapability::ConfigurationPublish, (context.clock)())?;
    let prepared = PreparedConfigurationPublication::capture(
        store,
        context.identity,
        context.inputs,
        &w.config.generation,
        (context.clock)(),
    )
    .await?;
    if prepared.configuration() != &w.config
        || prepared.owner_revision() != Some(w.owner_revision.as_str())
        || prepared.changed()
            && prepared.previous_configuration_revision() != w.previous_config_revision.as_deref()
    {
        return Err(conflict());
    }
    let disposition = if prepared.changed() {
        PublicationDisposition::Applied
    } else {
        PublicationDisposition::AlreadyCurrent
    };
    let configuration_revision = if prepared.changed() {
        w.publication_revision.clone()
    } else {
        prepared
            .previous_configuration_revision()
            .map(str::to_owned)
            .ok_or_else(invalid)?
    };
    let receipt = PublicationReceipt {
        v: 1,
        operation: "config_publish".into(),
        disposition,
        owner_id: prepared.configuration().owner_id.clone(),
        owner_epoch: prepared.configuration().owner_epoch.clone(),
        owner_revision: w.owner_revision,
        publication_revision: w.publication_revision.clone(),
        configuration_revision: Some(configuration_revision),
        configuration_generation: Some(prepared.configuration().generation.clone()),
        configuration_fingerprint: Some(prepared.configuration().configuration_fingerprint.clone()),
        source_revision: None,
        normalized_revision: None,
        pool_versionstamp: None,
        native_owner_id: None,
        account_id_hash: None,
        credential_version: None,
        native_generation: None,
    };
    prepared
        .publish_received(
            store,
            proof,
            (context.clock)(),
            &w.publication_revision,
            body,
        )
        .await?;
    Ok(receipt)
}

async fn apply_access<S: AuthorityStore>(
    store: &S,
    proof: &AuthenticatedNativeOwner,
    body: &[u8],
    context: &ApplyContext<'_>,
    w: crate::wire::AccessWire,
) -> Result<PublicationReceipt, GatewayError> {
    proof.require(NativeCapability::AccessPublish, (context.clock)())?;
    let registered = context
        .registered
        .iter()
        .find(|owner| owner.native_owner_id().0 == w.native_owner_id)
        .ok_or_else(|| {
            GatewayError::new(
                403,
                "Native access owner is not registered",
                Some("native_access_owner_unavailable"),
            )
        })?;
    let observed = w.document.native()?;
    if observed.routing_account_hash() != w.account_id_hash
        || observed.routing_credential_version() != w.credential_version
        || observed.generation_hash() != w.native_generation
    {
        return Err(invalid());
    }
    let source = ReceivedAccess {
        owner_id: w.native_owner_id.clone(),
        credentials: observed,
    };
    let prepared = PreparedAccessPublication::capture(
        store,
        &source,
        context.identity,
        registered,
        (context.clock)(),
        &w.publication_revision,
    )
    .await?;
    if prepared.checks[0].expected_revision.as_deref() != Some(&w.owner_revision)
        || !prepared.unchanged
            && (prepared.checks[1].expected_revision.as_deref() != Some(&w.source_revision)
                || prepared.checks[2].expected_revision != w.normalized_revision)
    {
        return Err(conflict());
    }
    let raw_revision = prepared.pool.pool_versionstamp.0.clone();
    let normalized_revision = if prepared.unchanged {
        prepared.checks[2]
            .expected_revision
            .clone()
            .unwrap_or_else(|| w.publication_revision.clone())
    } else {
        w.publication_revision.clone()
    };
    let receipt = PublicationReceipt {
        v: 1,
        operation: "access_publish".into(),
        disposition: if prepared.unchanged {
            PublicationDisposition::AlreadyCurrent
        } else {
            PublicationDisposition::Applied
        },
        owner_id: prepared.owner.owner_id.clone(),
        owner_epoch: prepared.owner.owner_epoch.clone(),
        owner_revision: w.owner_revision,
        publication_revision: w.publication_revision,
        configuration_revision: None,
        configuration_generation: None,
        configuration_fingerprint: None,
        source_revision: Some(raw_revision.clone()),
        normalized_revision: Some(normalized_revision),
        pool_versionstamp: Some(raw_revision),
        native_owner_id: Some(w.native_owner_id),
        account_id_hash: Some(w.account_id_hash),
        credential_version: Some(w.credential_version),
        native_generation: Some(w.native_generation),
    };
    prepared
        .publish_received(store, proof, (context.clock)(), body)
        .await?;
    Ok(receipt)
}

struct ReceivedAccess {
    owner_id: String,
    credentials: NativeCredentials,
}
impl AccessCredentialSource for ReceivedAccess {
    async fn read_current_access(
        &self,
        owner: &NativeOwnerId,
    ) -> Result<NativeCredentials, GatewayError> {
        if owner.0 != self.owner_id {
            return Err(invalid());
        }
        Ok(self.credentials.clone())
    }
}
