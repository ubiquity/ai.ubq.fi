use gateway_source::error::GatewayError;

#[derive(Clone, Debug)]
pub enum WaitOutcome {
    NotStarted {
        error: GatewayError,
    },
    Attempted {
        elapsed_ms: Result<i64, GatewayError>,
        result: Result<(), GatewayError>,
    },
}

#[derive(Clone, Debug)]
pub struct WaitObservation {
    pub elapsed_ms: Option<i64>,
    pub result: Result<(), GatewayError>,
    pub measurement: Result<(), GatewayError>,
}

impl WaitOutcome {
    pub fn into_observation(self) -> Result<WaitObservation, GatewayError> {
        match self {
            Self::NotStarted { error } => Err(error),
            Self::Attempted { elapsed_ms, result } => {
                let (elapsed_ms, measurement) = match elapsed_ms {
                    Ok(elapsed_ms) => (Some(elapsed_ms), Ok(())),
                    Err(error) => (None, Err(error)),
                };
                Ok(WaitObservation {
                    elapsed_ms,
                    result,
                    measurement,
                })
            }
        }
    }
}
