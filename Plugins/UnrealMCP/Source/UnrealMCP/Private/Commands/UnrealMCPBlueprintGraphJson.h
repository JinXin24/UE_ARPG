#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UEdGraphNode;
struct FEdGraphPinType;

// Graph serialization helpers shared by the Blueprint read and edit commands (defined in UnrealMCPBlueprintReadCommands.cpp)
namespace UnrealMCPBlueprintRead
{
    FString PinTypeToString(const FEdGraphPinType& Type);
    TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, const TMap<const UEdGraphNode*, FString>& NodeIds);
}
