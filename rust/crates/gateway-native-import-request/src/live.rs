//! Actor-local sealed live channel capability; wire stopped DATA cannot construct it.
use crate::bootstrap::InheritedCustodyChannel;
use gateway_authority::native_control::import::{
    AuthenticatedNativeImport, custody_session::ImportCustodyStartedRequest,
    effects::NativeImportStart, remote::AuthenticatedImportTerminal,
};
use gateway_authority_native_records::import_effects::NativeImportBinding;
use gateway_native_custody_io::protocol::{self as wire, unavailable};
use gateway_native_custody_wire::wire::{self as data, Frame, Header, Release, Stopped};
use gateway_native_import_context::context::NativeImportInputs;
use gateway_native_import_root::NativeRootView;
use gateway_source::error::GatewayError;
use std::{
    os::unix::fs::MetadataExt,
    time::{Duration, Instant},
};

/// Only the fixed actor's genuine original Start plus its inherited live peer can issue this.
/// ```compile_fail
/// use gateway_native_import_request::live::LiveImportCustody;
/// let forged: LiveImportCustody<'_> = serde_json::from_str("{}").unwrap();
/// ```
pub struct LiveImportCustody<'a> {
    channel: InheritedCustodyChannel,
    original_start: NativeImportStart<'a>,
    binding: ImportCustodyStartedRequest,
    challenge: String,
    epoch: String,
    root_device: u64,
    root_inode: u64,
    deadline: Instant,
    uncertain: bool,
    released: bool,
    publication_ref: Option<String>,
    publication_digest: Option<String>,
}
impl<'a> LiveImportCustody<'a> {
    pub fn begin_live_custody(
        mut channel: InheritedCustodyChannel,
        start: NativeImportStart<'a>,
        context: &NativeImportInputs<'_>,
    ) -> Result<Self, GatewayError> {
        let proof = context.original_proof();
        let original_body = context.original_body();
        context.check_document(proof, proof.metadata(), original_body)?;
        start.require_body(original_body, context.now_ms()?)?;
        if start.binding().action_ref != proof.metadata().action_ref
            || start.binding().operation_id != proof.metadata().operation_id
            || channel.next_sequence != 1
        {
            return Err(unavailable());
        }
        let binding = start.custody_request()?;
        let remaining = binding
            .deadline_ms
            .checked_sub(context.now_ms()?)
            .filter(|ms| *ms > 0 && *ms <= gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS)
            .ok_or_else(unavailable)?;
        let deadline = context
            .deadline()
            .min(Instant::now() + Duration::from_millis(remaining as u64));
        let challenge = wire::random_ref()?;
        let header = Header {
            v: 1,
            sequence: 1,
            session_ref: channel.session_ref.clone(),
            challenge: challenge.clone(),
            binding: data::copy_binding(&binding)?,
        };
        wire::send(
            &mut channel.socket,
            &Frame::Quiesce(header),
            deadline,
            Some(context.context),
        )?;
        let response = wire::receive(&mut channel.socket, deadline, Some(context.context))?;
        let Frame::Stopped(stopped) = response else {
            return Err(unavailable());
        };
        gateway_native_custody_wire::stopped_policy::verify_stopped_data(
            &stopped,
            &binding,
            &channel.session_ref,
            &challenge,
            1,
        )?;
        context.check_document(proof, proof.metadata(), original_body)?;
        channel.next_sequence = 2;
        Ok(Self {
            channel,
            original_start: start,
            binding,
            challenge,
            epoch: stopped.epoch,
            root_device: stopped.root_device,
            root_inode: stopped.root_inode,
            deadline,
            uncertain: false,
            released: false,
            publication_ref: None,
            publication_digest: None,
        })
    }
    pub fn publication_reference(&mut self) -> Result<String, GatewayError> {
        if self.uncertain || self.released {
            return Err(unavailable());
        }
        if self.publication_ref.is_none() {
            self.publication_ref = Some(wire::random_ref()?);
        }
        self.publication_ref.clone().ok_or_else(unavailable)
    }
    pub fn bind_publication(&mut self, digest: &str) -> Result<(), GatewayError> {
        if self.uncertain
            || self.released
            || !data::hash(digest)
            || self
                .publication_digest
                .as_deref()
                .is_some_and(|original| original != digest)
        {
            return Err(unavailable());
        }
        self.publication_digest = Some(digest.into());
        Ok(())
    }
    /// This is a live actual peer exchange after publication, not a historical
    /// B1 recheck or a caller-created Published/Boolean completion input.
    pub fn release_after_verified_terminal(
        &mut self,
        terminal: &AuthenticatedImportTerminal,
        context: &NativeImportInputs<'_>,
    ) -> Result<(), GatewayError> {
        if self.uncertain
            || self.released
            || Instant::now() >= self.deadline
            || terminal.binding() != self.binding()
            || self.publication_ref.as_deref()
                != Some(terminal.terminal().publication_revision.as_str())
            || self.publication_digest.as_deref()
                != Some(terminal.terminal().publication_request_digest.as_str())
            || terminal.observed_at_ms() > context.now_ms()?
        {
            return Err(unavailable());
        }
        context.check_document(
            context.original_proof(),
            context.original_proof().metadata(),
            context.original_body(),
        )?;
        self.original_start
            .require_body(context.original_body(), context.now_ms()?)?;
        self.uncertain = true;
        let sequence = self.channel.next_sequence;
        wire::send(
            &mut self.channel.socket,
            &Frame::ReleaseAfterVerifiedTerminal(Release {
                v: 1,
                sequence,
                session_ref: self.channel.session_ref.clone(),
                challenge: self.challenge.clone(),
                binding: data::copy_binding(&self.binding)?,
                publication_revision: terminal.terminal().publication_revision.clone(),
            }),
            self.deadline,
            Some(context.context),
        )?;
        let response = wire::receive(
            &mut self.channel.socket,
            self.deadline,
            Some(context.context),
        )?;
        let Frame::Released(header) = response else {
            return Err(unavailable());
        };
        if header.v != 1
            || header.sequence != sequence
            || header.session_ref != self.channel.session_ref
            || header.challenge != self.challenge
            || header.binding.canonical_bytes()? != self.binding.canonical_bytes()?
        {
            return Err(unavailable());
        }
        context.check_document(
            context.original_proof(),
            context.original_proof().metadata(),
            context.original_body(),
        )?;
        self.channel.next_sequence = sequence.checked_add(1).ok_or_else(unavailable)?;
        self.released = true;
        self.uncertain = false;
        Ok(())
    }
    pub(crate) fn binding(&self) -> &NativeImportBinding {
        self.original_start.binding()
    }
    pub fn check_live(
        &mut self,
        root: &NativeRootView<'_>,
        context: &NativeImportInputs<'_>,
    ) -> Result<(), GatewayError> {
        let proof = context.original_proof();
        let original_body = context.original_body();
        if self.uncertain || self.released || Instant::now() >= self.deadline {
            return Err(unavailable());
        }
        self.uncertain = true;
        context.check_document(proof, proof.metadata(), original_body)?;
        self.original_start
            .require_body(original_body, context.now_ms()?)?;
        root.recheck_live_root(
            self.original_start.binding(),
            (self.root_device, self.root_inode),
        )?;
        let sequence = self.channel.next_sequence;
        let header = Header {
            v: 1,
            sequence,
            session_ref: self.channel.session_ref.clone(),
            challenge: self.challenge.clone(),
            binding: data::copy_binding(&self.binding)?,
        };
        wire::send(
            &mut self.channel.socket,
            &Frame::CheckStopped(header),
            self.deadline,
            Some(context.context),
        )?;
        let response = wire::receive(
            &mut self.channel.socket,
            self.deadline,
            Some(context.context),
        )?;
        let Frame::Stopped(stopped) = response else {
            return Err(unavailable());
        };
        gateway_native_custody_wire::stopped_policy::verify_stopped_data(
            &stopped,
            &self.binding,
            &self.channel.session_ref,
            &self.challenge,
            sequence,
        )?;
        if stopped.epoch != self.epoch
            || stopped.root_device != self.root_device
            || stopped.root_inode != self.root_inode
        {
            return Err(unavailable());
        }
        self.channel.next_sequence = sequence.checked_add(1).ok_or_else(unavailable)?;
        context.check_document(proof, proof.metadata(), original_body)?;
        self.uncertain = false;
        Ok(())
    }
}
impl Drop for LiveImportCustody<'_> {
    fn drop(&mut self) {
        // Closing the actual endpoint releases no stopped-state claim and never restarts a daemon.
        let _ = self.channel.socket.shutdown(std::net::Shutdown::Both);
    }
}

impl LiveImportCustody<'_> {
    /// Ordinary command DATA; this copy cannot construct or renew live custody.
    pub fn copy_binding_data(&self) -> NativeImportBinding {
        self.binding().clone()
    }
    pub fn rotation_name_for_source(
        &self,
        current: &gateway_auth_credentials::native_credentials::NativeCredentials,
        next: &gateway_auth_credentials::native_credentials::NativeCredentials,
    ) -> Result<String, GatewayError> {
        let binding = self.binding();
        if current.routing_account_hash() != binding.account_id_hash
            || current.generation_hash() != binding.source_native_generation
            || next.account_id != current.account_id
            || next.external_user_id.is_some()
        {
            return Err(unavailable());
        }
        Ok(format!(".uos-import-{}.rotated", binding.action_ref))
    }
}
