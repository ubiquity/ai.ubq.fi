#include "uos/storage/value_compatibility.hpp"

// assertPortableKvValue is defined by rt (src/rt/kv_value.cpp) for the value
// model this module inspects; see the header. rt::KvValue has no accessor
// properties, so the module's TypeError "Bun KV requires materialized values
// rather than accessor properties" has no reachable C++ counterpart.
