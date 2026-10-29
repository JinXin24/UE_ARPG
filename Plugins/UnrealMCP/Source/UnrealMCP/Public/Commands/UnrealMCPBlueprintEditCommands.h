#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UBlueprint;

/**
 * Handler class for Blueprint editing commands: nodes, pins, layout, variables, functions and components.
 * Every edit runs inside an editor transaction (so Ctrl+Z works) and can recompile and report errors.
 */
class UNREALMCP_API FUnrealMCPBlueprintEditCommands
{
public:
    FUnrealMCPBlueprintEditCommands();

    // Handle blueprint edit commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

    // Names of all commands handled by this class
    static const TArray<FString>& GetCommandNames();

private:
    using FHandler = TSharedPtr<FJsonObject> (FUnrealMCPBlueprintEditCommands::*)(const TSharedPtr<FJsonObject>&);

    // Command name -> handler
    static const TMap<FString, FHandler>& GetHandlers();

    // Graph and node commands
    TSharedPtr<FJsonObject> HandleAddNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePasteNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPinDefaults(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMoveNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddComment(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAutoLayout(const TSharedPtr<FJsonObject>& Params);

    // Function graph commands
    TSharedPtr<FJsonObject> HandleAddFunction(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameGraph(const TSharedPtr<FJsonObject>& Params);

    // Variable and component commands
    TSharedPtr<FJsonObject> HandleAddVariables(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleModifyVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params);

    // Compile and save
    TSharedPtr<FJsonObject> HandleCompile(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSave(const TSharedPtr<FJsonObject>& Params);
};
