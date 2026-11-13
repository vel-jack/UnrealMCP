#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "UnrealMCPDataTableTestRow.generated.h"

USTRUCT()
struct FUnrealMCPDataTableTestRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY()
    FString Label;

    UPROPERTY()
    TSoftClassPtr<UObject> SpawnClass;
};
