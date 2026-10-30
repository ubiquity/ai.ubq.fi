use crate::wire::unavailable;
use gateway_source::error::GatewayError;

pub fn same_identity_data(
    expected: (&str, u64, u64),
    actual: (&str, u64, u64),
) -> Result<(), GatewayError> {
    if expected.0 != actual.0 || expected.1 != actual.1 || expected.2 != actual.2 {
        return Err(unavailable());
    }
    Ok(())
}
