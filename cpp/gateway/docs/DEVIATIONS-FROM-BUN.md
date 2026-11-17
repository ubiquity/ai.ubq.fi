# C++ gateway runtime deviations from Bun

## Unicode case mapping and default collation

The C++ runtime follows the Unicode data in the linked ICU release. On ICU 78.3, the existing Bun 1.4.0 case and collation vectors keep their expected values. The gateway uses ICU root-locale collation independently of machine locale.

Outside the old port's recorded collation repertoire, ICU supplies the order and canonical equivalences. For example, `가` and `가` compare equal, matching Bun 1.4.0. This extends collation to names in additional scripts and keeps tertiary ordering for catalog and schema names.
