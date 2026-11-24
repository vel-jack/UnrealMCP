#pragma once

#include "Misc/EngineVersionComparison.h"
#include "UObject/ObjectMacros.h"

namespace UnrealMCPTest
{
    // Fixture cleanup deliberately moves objects away from their original package linkers.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    inline constexpr ERenameFlags TransientRenameLinkerFlag = REN_ForceNoResetLoaders;
#else
    inline constexpr ERenameFlags TransientRenameLinkerFlag = REN_AllowPackageLinkerMismatch;
#endif
}
