Build branch (not part of the gateway): rebuilds the Raspberry Pi OS kernel 6.12.109+rpt for the owner's Pi Zero 2 W
with Landlock enabled, so the native gateway's sandbox can run there. Same source package and configuration as the
running kernel; the only changes are CONFIG_SECURITY_LANDLOCK=y, CONFIG_LSM="landlock" and the release suffix.
