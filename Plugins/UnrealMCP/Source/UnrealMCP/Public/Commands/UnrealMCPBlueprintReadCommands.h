#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for read-only Blueprint inspection commands
 * (listing Blueprint assets and dumping their variables, components and graph logic)
 */
class UNREALMCP_API FUnrealMCPBlueprintReadCommands
{
public:
    FUnrealMCPBlueprintReadCommands();

    // Handle blueprint read commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Specific blueprint read command handlers
    TSharedPtr<FJsonObject> HandleListBlueprints(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReadBlueprint(const TSharedPtr<FJsonObject>& Params);
};
