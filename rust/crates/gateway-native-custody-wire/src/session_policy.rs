use crate::wire::{Header, HeaderView, unavailable};
use gateway_authority::native_control::import::{
    custody_session::ImportCustodyStartedRequest, remote::VerifiedCurrentImportTerminal,
};
use gateway_source::error::GatewayError;

pub struct TerminalObservationData<'a> {
    request: &'a ImportCustodyStartedRequest,
    publication_revision: &'a str,
    observed_at_ms: i64,
    deadline_ms: i64,
}

impl<'a> TerminalObservationData<'a> {
    pub fn observe(fresh: &'a VerifiedCurrentImportTerminal) -> Self {
        Self {
            request: fresh.request(),
            publication_revision: fresh.publication_revision(),
            observed_at_ms: fresh.observed_at_ms(),
            deadline_ms: fresh.deadline_ms(),
        }
    }
}

pub struct CustodySessionData {
    binding: Vec<u8>,
    session_ref: String,
    challenge: String,
    stopped: bool,
    released: bool,
}

impl CustodySessionData {
    pub fn bind(request: Header) -> Result<Self, GatewayError> {
        Ok(Self {
            binding: request.binding.canonical_bytes()?,
            session_ref: request.session_ref,
            challenge: request.challenge,
            stopped: false,
            released: false,
        })
    }
    pub fn mark_stopped_data(&mut self) {
        self.stopped = true;
    }
    pub fn require_stopped_data(&self) -> Result<(), GatewayError> {
        if !self.stopped || self.released {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn require_recheck_data(
        &self,
        header: HeaderView<'_>,
        fresh: &ImportCustodyStartedRequest,
    ) -> Result<(), GatewayError> {
        if !self.stopped
            || self.released
            || header.session_ref != self.session_ref
            || header.challenge != self.challenge
            || header.binding.canonical_bytes()? != self.binding
            || fresh.canonical_bytes()? != self.binding
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn require_release_after_deadline_data(
        &self,
        header: HeaderView<'_>,
        fresh: TerminalObservationData<'_>,
        publication: Option<&str>,
        now: i64,
    ) -> Result<(), GatewayError> {
        if header.session_ref != self.session_ref
            || header.challenge != self.challenge
            || header.binding.canonical_bytes()? != self.binding
            || fresh.request.canonical_bytes()? != self.binding
            || publication != Some(fresh.publication_revision)
            || fresh.observed_at_ms > now
            || fresh.deadline_ms <= now
            || fresh.deadline_ms != header.binding.deadline_ms
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn mark_released_data(&mut self) {
        self.released = true;
    }
}
