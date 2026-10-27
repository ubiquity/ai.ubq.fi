use serde::{Deserialize, Serialize};
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SupervisorState {
    Active,
    Idle,
    Stale,
    Unknown,
    SystemError,
}

pub fn classify_state(runtime: Option<&str>, turn: Option<&str>, loaded: bool) -> SupervisorState {
    let terminal = turn.is_some_and(|status| {
        matches!(
            status,
            "completed"
                | "interrupted"
                | "failed"
                | "declined"
                | "cancelled"
                | "canceled"
                | "errored"
                | "aborted"
        )
    });
    match runtime {
        Some("active") => SupervisorState::Active,
        Some("systemError") => SupervisorState::SystemError,
        Some("idle") => SupervisorState::Idle,
        Some("notLoaded") => {
            if turn.is_some() && !terminal {
                SupervisorState::Stale
            } else if terminal {
                SupervisorState::Idle
            } else {
                SupervisorState::Unknown
            }
        }
        _ => {
            if turn.is_some() && !terminal {
                if loaded {
                    SupervisorState::Active
                } else {
                    SupervisorState::Stale
                }
            } else {
                SupervisorState::Unknown
            }
        }
    }
}
