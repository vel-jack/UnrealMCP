#pragma once

// Reconstruction/compile/rollback engine behind RefreshBlueprintCallSites.
//
// This is split out from the tool so it can be driven directly by automation tests. The tool itself
// discovers call sites through the SQLite project index, and that index is built from
// AssetRegistry.GetAllAssets, which never sees the transient unsaved packages automation fixtures
// create. Keeping the engine separate from the index query is therefore the only way to exercise the
// compile-failure rollback path in a test without inventing a test-only field on the public schema.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UnrealMCP::CallSiteRefresh
{
    // One indexed call site: the Blueprint that contains the call node, its graph identifier as the
    // index stores it ("Name::GraphGuid::AssetPath:Name"), and the call node's GUID.
    struct FCallSiteRow
    {
        FString BlueprintObjectPath;
        FString GraphName;
        FString NodeGuid;
    };

    struct FOptions
    {
        bool bDryRun = false;
        bool bCompileCallers = true;
        bool bRefreshIndex = false;
        FString OperationId;
    };

    // Reconstructs every listed call site inside one caller Blueprint.
    //
    // Returns true when the asset completed successfully. On a post-reconstruction compile failure the
    // edit is rolled back, each node is re-resolved to confirm its pin shape was restored, the package
    // dirty flag is put back, and the measured outcome is reported through OutResult.
    bool RefreshCallSitesInBlueprint(
        const FString& CallerObjectPath,
        const TArray<FCallSiteRow>& CallerRows,
        const FOptions& Options,
        const TSharedRef<FJsonObject>& OutResult);
}
