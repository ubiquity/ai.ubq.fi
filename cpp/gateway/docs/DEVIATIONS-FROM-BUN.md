# C++ gateway runtime deviations from Bun

## Unicode case mapping and default collation

The C++ runtime follows the Unicode data in the linked ICU release. On ICU 78.3, the existing Bun 1.4.0 case and collation vectors keep their expected values. The gateway uses ICU root-locale collation independently of machine locale.

Outside the old port's recorded collation repertoire, ICU supplies the order and canonical equivalences. With ICU 78.3, Deseret U+10400 sorts before the lone surrogate U+D83D, and `가` and `가` compare equal. Both results match Bun 1.4.0. This extends collation to names in additional scripts and keeps tertiary ordering for catalog and schema names.
