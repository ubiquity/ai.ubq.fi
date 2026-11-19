# Native Linux operations

`launch-native.sh` takes one absolute path to a private launch manifest. It clears inherited loader overrides before executing the sealed gateway. Startup verifies the manifest, packaged files and host libraries, then applies Linux confinement before opening state or binding its loopback listener. [Runtime and launch schema](../docs/RUNTIME.md)

The draft systemd unit uses an explicit operator-owned `EnvironmentFile=`. Render its user, state, package and environment paths for the approved host. Ports 7999 and 8000 are reserved. Use the separate observation template for synthetic state and public-read replay. [Side-by-side runbook](../docs/SIDE-BY-SIDE.md) · [Owner cutover checklist](../docs/CUTOVER.md)
