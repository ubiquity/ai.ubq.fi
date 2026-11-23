R"W06C3([
  {
    "name": "refresh_unchanged",
    "results": [
      {
        "status": "inconclusive",
        "completed_cycles": 0,
        "inconclusive_reason": "refresh_unchanged"
      }
    ],
    "inferenceCalls": 5,
    "refreshes": 1,
    "atomicCalls": 11,
    "state_next_cycle": null,
    "evidence_outcome": "inconclusive",
    "evidence_cycles": 1,
    "published_scope": false
  },
  {
    "name": "cycle_disagreement",
    "results": [
      {
        "status": "in_progress",
        "completed_cycles": 1
      },
      {
        "status": "in_progress",
        "completed_cycles": 2
      },
      {
        "status": "inconclusive",
        "completed_cycles": 3,
        "inconclusive_reason": "cycle_disagreement"
      }
    ],
    "inferenceCalls": 30,
    "refreshes": 3,
    "atomicCalls": 47,
    "state_next_cycle": null,
    "evidence_outcome": "inconclusive",
    "evidence_cycles": 3,
    "published_scope": false
  },
  {
    "name": "session_auth_drift",
    "results": [
      {
        "status": "in_progress",
        "completed_cycles": 1
      },
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment state changed concurrently."
        }
      }
    ],
    "inferenceCalls": 10,
    "refreshes": 1,
    "atomicCalls": 18,
    "state_next_cycle": 2,
    "evidence_outcome": "in_progress",
    "evidence_cycles": 1,
    "published_scope": false
  },
  {
    "name": "lost_cycle_lease",
    "results": [
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment lost its lease."
        }
      }
    ],
    "inferenceCalls": 1,
    "refreshes": 0,
    "atomicCalls": 3,
    "state_next_cycle": 1,
    "evidence_outcome": null,
    "evidence_cycles": null,
    "published_scope": false
  },
  {
    "name": "state_changed",
    "results": [
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment state changed concurrently."
        }
      }
    ],
    "inferenceCalls": 10,
    "refreshes": 1,
    "atomicCalls": 16,
    "state_next_cycle": 1,
    "evidence_outcome": null,
    "evidence_cycles": null,
    "published_scope": false
  },
  {
    "name": "intermediate_conflicts",
    "results": [
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment evidence could not be persisted."
        }
      }
    ],
    "inferenceCalls": 10,
    "refreshes": 1,
    "atomicCalls": 20,
    "state_next_cycle": null,
    "evidence_outcome": "failed",
    "evidence_cycles": 0,
    "published_scope": false
  },
  {
    "name": "final_conflicts",
    "results": [
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment final result could not be persisted."
        }
      }
    ],
    "inferenceCalls": 1,
    "refreshes": 0,
    "atomicCalls": 9,
    "state_next_cycle": 1,
    "evidence_outcome": null,
    "evidence_cycles": null,
    "published_scope": false
  },
  {
    "name": "promotion_conflict",
    "results": [
      {
        "status": "in_progress",
        "completed_cycles": 1
      },
      {
        "status": "in_progress",
        "completed_cycles": 2
      },
      {
        "status": "inconclusive",
        "completed_cycles": 3,
        "inconclusive_reason": "target_catalog_drift"
      }
    ],
    "inferenceCalls": 30,
    "refreshes": 3,
    "atomicCalls": 48,
    "state_next_cycle": null,
    "evidence_outcome": "inconclusive",
    "evidence_cycles": 3,
    "published_scope": false
  },
  {
    "name": "promotion_final_conflicts",
    "results": [
      {
        "status": "in_progress",
        "completed_cycles": 1
      },
      {
        "status": "in_progress",
        "completed_cycles": 2
      },
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment final result could not be persisted."
        }
      }
    ],
    "inferenceCalls": 30,
    "refreshes": 3,
    "atomicCalls": 51,
    "state_next_cycle": 4,
    "evidence_outcome": "ready_to_promote",
    "evidence_cycles": 3,
    "published_scope": true
  },
  {
    "name": "promotion_ready_resume",
    "results": [
      {
        "status": "in_progress",
        "completed_cycles": 1
      },
      {
        "status": "in_progress",
        "completed_cycles": 2
      },
      {
        "error": {
          "name": "PromptCacheScopeExperimentFailedError",
          "message": "Prompt-cache scope experiment final result could not be persisted."
        }
      },
      {
        "status": "completed",
        "completed_cycles": 3,
        "scope": {
          "probe_profile": "responses_implicit_input_text_keyed_cycle_isolated_v5",
          "account_slots": "account_scoped",
          "token_refresh": "preserved",
          "conversation_id": "independent",
          "effective_model": "gpt-5.6-cache-scope-fixture"
        }
      }
    ],
    "inferenceCalls": 30,
    "refreshes": 3,
    "atomicCalls": 54,
    "state_next_cycle": null,
    "evidence_outcome": "completed",
    "evidence_cycles": 3,
    "published_scope": true
  }
]
)W06C3"
