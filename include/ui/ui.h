// Umbrella header for the Axe C API. Structs only gain fields at their end, so initialize them
// with designated initializers (`{.decls = d, .count = 2}`); positional initializers are
// unsupported except for the fixed pairs (docs/c-api.md).
#pragma once

#include "ui/app.h"
#include "ui/handle.h"
#include "ui/hooks.h"
#include "ui/result.h"
#include "ui/script.h"
#include "ui/wire.h"
