# Sentinel Review Backlog

Native Codex review P2 and P3 findings are durable, non-blocking backlog items. The Sentinel deduplicates rows by
fingerprint and updates the latest observation, affected SHA, location, and disposition when it sees a finding again.

| Fingerprint | Severity | First observation | Latest observation | Affected SHA | Location | Finding | Disposition |
| ----------- | -------- | ----------------- | ------------------ | ------------ | -------- | ------- | ----------- |
