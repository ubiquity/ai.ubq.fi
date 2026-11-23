# Live heap after streamed Responses and catalog fetches

- [ ] Reproduce the reported growth and measure buffered and unversioned controls.
- [ ] Record the growing allocation sites and their owners before changing request lifetimes.
- [ ] Fix retention at the source and account for the identified process-wide structures.
- [ ] Demonstrate the Release regression gate failing before the fix and passing after it.
- [ ] Record the 500-request comparisons and scoped Release and ASan results.

The integration base is `d81580d6d2cae4b0766c8f1536c9df0fb5b4a7ad` with tree `18bb1fcebeecc2a950ed495594198d81bb3dcc25`. The runner is Ubuntu 26.04.1 on Linux ARM64 with four CPUs. The reported deployment grew from 8 to 43 MiB of live allocator bytes while free blocks stayed at 8–12 MiB. The local fixture will exercise the same API-key route with a loopback Lithos upstream and the same exact-version Codex catalog shape.
