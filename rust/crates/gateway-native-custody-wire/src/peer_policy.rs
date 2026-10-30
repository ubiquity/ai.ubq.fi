use crate::wire::{Frame, Header, HeaderView, Stopped, unavailable};
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_source::error::GatewayError;

pub struct CustodyCommandData {
    frame: Frame,
    digest: String,
}

impl CustodyCommandData {
    pub fn header(&self) -> HeaderView<'_> {
        self.frame.header()
    }
    pub fn is_quiesce(&self) -> bool {
        matches!(self.frame, Frame::Quiesce(_))
    }
    pub fn is_check(&self) -> bool {
        matches!(self.frame, Frame::CheckStopped(_))
    }
    pub fn publication_revision(&self) -> Option<&str> {
        if let Frame::ReleaseAfterVerifiedTerminal(release) = &self.frame {
            Some(&release.publication_revision)
        } else {
            None
        }
    }
}

pub struct CustodyPeerData {
    session_ref: String,
    next_sequence: u64,
    pending: Option<String>,
    first_consumed: bool,
    binding: Option<Vec<u8>>,
    challenge: Option<String>,
}

impl CustodyPeerData {
    pub fn fresh(session_ref: String) -> Self {
        Self {
            session_ref,
            next_sequence: 1,
            pending: None,
            first_consumed: false,
            binding: None,
            challenge: None,
        }
    }
    pub fn require_idle(&self) -> Result<(), GatewayError> {
        if self.pending.is_some() {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn accept_received(&mut self, frame: Frame) -> Result<CustodyCommandData, GatewayError> {
        let h = frame.header();
        if h.sequence != self.next_sequence
            || h.session_ref != self.session_ref
            || !matches!(
                frame,
                Frame::Quiesce(_)
                    | Frame::CheckStopped(_)
                    | Frame::Cancel(_)
                    | Frame::ReleaseAfterVerifiedTerminal(_)
            )
        {
            return Err(unavailable());
        }
        let binding = h.binding.canonical_bytes()?;
        match (&self.binding, &self.challenge) {
            (Some(expected), Some(challenge))
                if *expected == binding && challenge == h.challenge => {}
            (None, None) if matches!(frame, Frame::Quiesce(_) | Frame::Cancel(_)) => {
                self.binding = Some(binding);
                self.challenge = Some(h.challenge.into());
            }
            _ => return Err(unavailable()),
        }
        if self.first_consumed && matches!(frame, Frame::Quiesce(_)) {
            return Err(unavailable());
        }
        let digest = sha256_hex(&frame.encode()?);
        self.pending = Some(digest.clone());
        Ok(CustodyCommandData { frame, digest })
    }
    pub fn require_pending(&self, command: &CustodyCommandData) -> Result<(), GatewayError> {
        if self.pending.as_deref() != Some(command.digest.as_str())
            || sha256_hex(&command.frame.encode()?) != command.digest
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn consume_first(&mut self, command: &CustodyCommandData) -> Result<Header, GatewayError> {
        if self.first_consumed || !command.is_quiesce() {
            return Err(unavailable());
        }
        self.first_consumed = true;
        command.header().copy_data()
    }
    pub fn commit_reply(&mut self) -> Result<(), GatewayError> {
        self.next_sequence = self.next_sequence.checked_add(1).ok_or_else(unavailable)?;
        self.pending.take();
        Ok(())
    }
}

pub fn stopped_reply(
    header: HeaderView<'_>,
    identity: (String, u64, u64),
) -> Result<Frame, GatewayError> {
    let h = header.copy_data()?;
    Ok(Frame::Stopped(Stopped {
        v: 1,
        sequence: h.sequence,
        session_ref: h.session_ref,
        challenge: h.challenge,
        binding: h.binding,
        epoch: identity.0,
        root_device: identity.1,
        root_inode: identity.2,
    }))
}

pub fn released_reply(command: &CustodyCommandData) -> Result<Frame, GatewayError> {
    if command.publication_revision().is_none() {
        return Err(unavailable());
    }
    Ok(Frame::Released(command.header().copy_data()?))
}

pub fn unknown_reply(header: HeaderView<'_>) -> Result<Frame, GatewayError> {
    Ok(Frame::Unknown(header.copy_data()?))
}
