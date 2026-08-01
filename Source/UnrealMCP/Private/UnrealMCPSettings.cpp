#include "UnrealMCPSettings.h"

UUnrealMCPSettings::UUnrealMCPSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("UnrealMCP");
}

FName UUnrealMCPSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}
