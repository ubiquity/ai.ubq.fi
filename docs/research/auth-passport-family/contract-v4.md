# auth-passport-family contract v4 — active continuation

Extends v2/v3; the original complete family objective and acceptance gates remain active. This is not a completion receipt.
Root's storage recovery and boundary adjudication are now the controlling execution/ownership constraints.

- Future first-party targets are distinct, owner-marked overlay directories:
  `/workspace/.ai-rust-builds/auth-passport-family/feature-ad7452359b8` and
  `/workspace/.ai-rust-builds/auth-passport-family/candidate-ad7452359b8`.
  Each exact `.uos-worktree-owner.json` binds its actual physical `.../<worktree>/rust` source root (Cargo workspace).
  Existing `/tmp/.../tools/target` remains quiescent and preserved. No target migration or other worktree cache reuse.
- Immutable pinned Rust/Bun and public registry sources/archives are reused read-only. No duplicate tool installs.
  The task-local Cargo index/cache metadata and temporary directory remain separate.
- Every new compiler/Clippy/test-driver/Worker build requires C ownership in
  `/tmp/ai-rust-cloud-control/compiler-lease.json` AND kernel
  `flock -x /tmp/ai-rust-cloud-control/compiler.lock`. The event watcher is persistent; no new compiler while another owner holds the lease.
  `gate.py` refuses to launch without the C lease and exact target owner marker, and records lease, argv, cwd, host, paths/hashes and output.
- C owns only discovery and the frozen SourceActor/principal/audit/live proposal outside auth. Proposal v1 is frozen at
  22c648bdd999b5999ee211ce4191e3a649d2fb8d. Root authority writer `rust_authority_size` owns actor semantics; root will
  lease one exact shared-foundation writer. No external semantic implementation or generic waiver is inferred.
  The legacy JWT SourceActor remains unchanged. App actors need new explicit provenance/version/currentness, not optional legacy proof fields.
- Independent auth work includes the whole bounded family split, real-crypto public passkey fixtures, no-expiry Passport
  session/browser implementations, strict source UTF16 key-ID/claim bounds and controlled Worker conformance definitions.
  Shared Worker harness registration remains mechanical and must bind the actual compiled Worker/common durable authority.
  Definitions, typed fixture callbacks or mocked upstream health are not executed runtime acceptance.

All pre-recovery compiler/parser/Clippy receipts are historical and stale for the current changed source tree. Input hash,
ancestry and immutable-tool observations remain separately scoped facts. Strict Clippy's old Cargo exit 0 did not pass its
staged comparator; source fixes require fresh actual diagnostics and the unchanged comparator. No allowance or context-hash transfer.
The old unshare failure was `/proc/self/uid_map` read-only in the tool sandbox, not proof that the host kernel supports no
user namespaces. At the leased gate, use the actual supported repository boundary with permitted execution access and
report its actual result; never run tests outside confinement or spoof an architecture. Mac/native/client/browser acceptance stays parent-owned.

Latest source and composed-candidate refs are recorded by receipts, not hard-coded as immutable future tips here. Candidate
ancestry merges preserve exact baseline, M67, admin and M60 tips; auth feature and external mechanical commits stay distinct.
Normal auth-private source/compile/lint fixes continue autonomously. Dependent persistent identity integration awaits the
frozen foundation/authority boundary commit. No deployed/provider/credential/native activation is authorized.
