//! Ordinary existing managed-publication response DATA; never a native durability or live proof.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NativeManagedPublicationDisposition {
    Applied,
    AlreadyCurrent,
}
pub struct NativeManagedPublication {
    pub disposition: NativeManagedPublicationDisposition,
    pub owner_id: String,
    pub owner_epoch: String,
    pub action_ref: String,
    pub operation_id: String,
    pub native_owner_id: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub publication_revision: String,
    pub current_source_revision: String,
    pub current_normalized_revision: String,
    pub previous_native_source_generation: String,
    pub native_source_generation: String,
    pub access_generation: String,
}
