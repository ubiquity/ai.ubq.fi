//! One borrowed observation pass with the existing ordered deferred finishes.
use crate::{
    paid::PaidAudit,
    usage::{ApiAudit, KernelAudit},
};
use gateway_admin_data::report::Audit;
use gateway_source::error::GatewayError;
use gateway_migration_ledger::rows::ObservedRow;
use std::collections::BTreeSet;

pub fn observe_coupled<T>(
    now: i64,
    visit: impl FnOnce(
        &mut dyn FnMut(&ObservedRow),
    ) -> Result<(T, BTreeSet<String>, BTreeSet<String>), GatewayError>,
) -> Result<(T, Audit), GatewayError> {
    let mut api = ApiAudit::new();
    let mut kernel = KernelAudit::new();
    let mut paid = PaidAudit::new(now);
    let (payload, known, unlimited) = {
        let mut observe = |row: &ObservedRow| {
            api.accept(row);
            kernel.accept(row);
            paid.accept(row);
        };
        visit(&mut observe)?
    };
    let mut audit = api.finish(&known)?;
    audit.merge(kernel.finish()?);
    audit.merge(paid.finish(&known, &unlimited)?);
    Ok((payload, audit))
}
