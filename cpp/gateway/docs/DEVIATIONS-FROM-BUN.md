# C++ gateway runtime deviations from Bun

## Unicode case mapping and default collation

The C++ runtime follows the Unicode data in the linked ICU release. On ICU 78.3, the existing Bun 1.4.0 case and collation vectors keep their expected values. The gateway uses ICU root-locale collation independently of machine locale.

Outside the old port's recorded collation repertoire, ICU supplies the order and canonical equivalences. With ICU 78.3, Deseret U+10400 sorts before the lone surrogate U+D83D, and `가` and `가` compare equal. Both results match Bun 1.4.0. This extends collation to names in additional scripts and keeps tertiary ordering for catalog and schema names.

## Child processes

Subprocess signal names follow the host. On macOS, a child terminated by `SIGUSR1` reports `SIGUSR1` and resolves `exited()` to 158. On Linux, the native name for signal 16 is `SIGSTKFLT`. Invalid signal inputs still throw `TypeError`, with a shorter diagnostic.

Executable and working-directory failures expose the OS errno and executable path with the operation name `spawn`. The adapter keeps the explicit environment and PATH lookup policy, preserves the original argv, and reports terminating signals separately from ordinary exit codes.
