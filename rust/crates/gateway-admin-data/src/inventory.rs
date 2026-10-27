use gateway_source::error::GatewayError;
use gateway_migration_ledger::rows::ObservedRow;

/// Replayable complete inventory. Each visit starts at the original first row and
/// must validate its complete EOF before success; callback/I/O errors propagate.
pub trait AuditInventory {
    fn visit(
        &self,
        row: &mut dyn FnMut(&ObservedRow) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError>;
}
pub struct SliceInventory<'a>(pub &'a [ObservedRow]);
impl AuditInventory for SliceInventory<'_> {
    fn visit(
        &self,
        row: &mut dyn FnMut(&ObservedRow) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError> {
        for value in self.0 {
            row(value)?;
        }
        Ok(())
    }
}
