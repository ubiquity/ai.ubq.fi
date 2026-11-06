//! The owned allocation is transparent on the private serialized wire.
use gateway_authority_lifecycle::model::{Counts, Identity, Phase, Reply, Status};
use gateway_authority_lifecycle::policy;
use gateway_worker_authority::wire::StoreResponse;
use serde_json::json;

type Outcome = Result<(), Box<dyn std::error::Error>>;

#[test]
fn boxed_coordination_preserves_the_existing_tag_and_payload_shape() -> Outcome {
    let encoded = serde_json::to_vec(&StoreResponse::Coordination {
        reply: Box::new(Reply::Acquired { serving_epoch: 7 }),
    })?;
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&encoded)?,
        json!({"operation":"coordination","reply":{"operation":"acquired","serving_epoch":7}})
    );
    let decoded: StoreResponse = serde_json::from_slice(&encoded)?;
    match decoded {
        StoreResponse::Coordination { reply } => match *reply {
            Reply::Acquired { serving_epoch } => assert_eq!(serving_epoch, 7),
            _ => return Err("Decoded a different coordination payload".into()),
        },
        _ => return Err("Decoded a different response operation".into()),
    }
    Ok(())
}

#[test]
fn boxed_status_preserves_the_large_nested_payload_without_a_wrapper_field() -> Outcome {
    let gate = policy::new_gate(
        Identity {
            object_id: "a".repeat(64),
            source_sha: "b".repeat(40),
            source_tree: "c".repeat(40),
        },
        false,
    )?;
    let status = Status {
        gate,
        counts: Counts {
            roots: 0,
            native_operations: 0,
            native_joins: 0,
            import_frames: 0,
            blocked: 0,
        },
    };
    let expected = json!({"operation":"coordination","reply":{"operation":"status","status":serde_json::to_value(&status)?}});
    let encoded = serde_json::to_vec(&StoreResponse::Coordination {
        reply: Box::new(Reply::Status { status }),
    })?;
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&encoded)?,
        expected
    );
    let decoded: StoreResponse = serde_json::from_slice(&encoded)?;
    match decoded {
        StoreResponse::Coordination { reply } => match *reply {
            Reply::Status { status } => {
                assert_eq!(status.gate.phase, Phase::Provisioning);
                assert_eq!(status.gate.identity.object_id, "a".repeat(64));
                assert_eq!(status.gate.drain_revision, 0);
                assert!(status.gate.last_cancellation.is_none());
                assert!(status.counts.empty());
            }
            _ => return Err("Decoded a different large coordination payload".into()),
        },
        _ => return Err("Decoded a different large response operation".into()),
    }
    Ok(())
}
