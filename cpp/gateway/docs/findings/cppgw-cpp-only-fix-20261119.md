# C++-only gateway follow-up

The starting commit is `f92b14a97818ff81bf95cbfb2642e8ff13b4ab13` with tree `9a456b2566d6539ea817ed9c21b2b0ce7c297abf`. The deletion audit compares it with STATE A, `ca045a83ebaab6c50c20567d1a825638abac0891`.

The cleanup report records two defects for this job. The passkey fuzz builds still name a deleted source, and the portable gateway reports `unknown` for `git_sha`. The deletion audit, release identity fix and local verification are pending.

## Verification in progress

The runner is Ubuntu 24.04.5 on ARM64 with four CPUs, GCC 14.2.0 and Docker 28.0.4. `sudo -n unshare --net -- true` succeeded. Toolchain preparation and test evidence will be recorded here as they complete.
