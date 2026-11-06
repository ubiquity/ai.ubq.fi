//! Core-state manifest accounting refusals (moved from the in-crate test module).
mod manifest {
    use gateway_migration::core_state::{
        CORE_STATE_DO_ROW_WRITE_LIMIT, CoreStateManifest, CoreStateRuleCount,
    };

    fn valid_manifest() -> CoreStateManifest {
        CoreStateManifest {
            v: 1,
            source_git_sha: "a".repeat(40),
            captured_at_ms: 1,
            source_rows: 1,
            selected_rows: 1,
            excluded_rows: 0,
            included: vec![CoreStateRuleCount {
                rule: "fixture".into(),
                rows: 1,
            }],
            excluded: vec![],
            derived_rows: 0,
            manifest_rows: 1,
            do_row_writes: 3,
            do_row_write_limit: CORE_STATE_DO_ROW_WRITE_LIMIT,
            within_free_limit: true,
            current_api_windows: 1,
            current_paid_requests: 0,
            current_paid_unresolved: 0,
            current_paid_pending: 0,
        }
    }

    #[test]
    fn missing_current_quota_or_pending_evidence_is_refused() {
        let mut missing_window = valid_manifest();
        missing_window.current_api_windows = 0;
        assert!(missing_window.validate().is_err());

        let mut missing_pending = valid_manifest();
        missing_pending.current_paid_unresolved = 1;
        assert!(missing_pending.validate().is_err());
    }

    #[test]
    fn write_budget_never_accepts_the_free_tier_boundary() {
        let mut manifest = valid_manifest();
        manifest.source_rows = 50_000;
        manifest.selected_rows = 50_000;
        manifest.included[0].rows = 50_000;
        manifest.do_row_writes = 100_001;
        manifest.within_free_limit = false;
        assert!(manifest.validate().is_err());
    }
}
