use gateway_native_confinement::file_observation::Facts;
use gateway_source::error::GatewayError;

pub fn require_prepared_file(facts: &Facts, writer: u32) -> Result<(), GatewayError> {
    if facts.uid != writer
        || facts.mode & 0o170000 != 0o100000
        || facts.mode & 0o077 != 0
        || facts.links != 1
    {
        return Err(unavailable());
    }
    Ok(())
}

pub fn require_stopped_auth_file(facts: &Facts, writer: u32) -> Result<(), GatewayError> {
    if facts.uid != writer || facts.mode & 0o170000 != 0o100000 || facts.links != 1 {
        return Err(unavailable());
    }
    Ok(())
}

pub fn require_generation_file(facts: &Facts, writer: u32) -> Result<(), GatewayError> {
    if facts.mode & 0o170000 != 0o100000
        || facts.uid != writer
        || facts.links != 1
        || facts.size > 1_048_576
    {
        return Err(unavailable());
    }
    Ok(())
}

pub fn require_epoch_file_shape(facts: &Facts) -> Result<(), GatewayError> {
    if facts.mode & 0o170000 != 0o100000
        || facts.mode & 0o077 != 0
        || facts.links != 1
        || facts.size > 2048
    {
        return Err(unavailable());
    }
    Ok(())
}

fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
