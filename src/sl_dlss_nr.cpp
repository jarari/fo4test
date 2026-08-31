#include "sl_dlss_nr.h"

// Guard the recovered ABI at its compilation boundary.  These assertions are
// intentionally kept out of Streamline.cpp so replacing the provisional SDK
// declaration is a single-file migration.
static_assert(sizeof(sl::BaseStructure) == 32);
static_assert(sizeof(sl::DLSSNROptions) == 72);
static_assert(alignof(sl::DLSSNROptions) == alignof(sl::BaseStructure));
