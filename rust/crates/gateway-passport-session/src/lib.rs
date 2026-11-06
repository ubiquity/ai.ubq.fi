//! Revocable persistent Passport app sessions, mirroring `src/auth/passport-sessions.ts`.
//! The browser holds an opaque local secret. The authority store holds only that
//! secret's SHA-256 key and the app-scoped upstream grant: no expiry and no saved
//! role. Every use introspects the grant against Passport's current user.

pub mod binding;
pub mod call;
pub mod record;
pub mod service;
