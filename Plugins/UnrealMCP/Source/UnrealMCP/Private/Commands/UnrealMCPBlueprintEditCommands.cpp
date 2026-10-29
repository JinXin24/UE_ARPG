#include "Commands/UnrealMCPBlueprintEditCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "UnrealMCPBlueprintGraphJson.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "K2Node.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetArrayLibrary.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "Layout/SlateRect.h"

namespace UnrealMCPBlueprintEdit
{
    using namespace UnrealMCPBlueprintRead;

    TArray<UEdGraph*> GetAllGraphs(UBlueprint* Blueprint)
    {
        TArray<UEdGraph*> Graphs;
        Graphs.Append(Blueprint->UbergraphPages);
        Graphs.Append(Blueprint->FunctionGraphs);
        Graphs.Append(Blueprint->MacroGraphs);
        Graphs.Append(Blueprint->DelegateSignatureGraphs);
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            Graphs.Append(Interface.Graphs);
        }
        Graphs.RemoveAll([](const UEdGraph* Graph) { return Graph == nullptr; });
        return Graphs;
    }

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
    {
        const FString Wanted = GraphName.IsEmpty() ? FString(TEXT("EventGraph")) : GraphName;
        for (UEdGraph* Graph : GetAllGraphs(Blueprint))
        {
            if (Graph->GetName().Equals(Wanted, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    FString GraphNamesList(UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (UEdGraph* Graph : GetAllGraphs(Blueprint))
        {
            Names.Add(Graph->GetName());
        }
        return FString::Join(Names, TEXT(", "));
    }

    UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetName().Equals(NodeName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        // Also accept a node GUID, as returned by the older node commands
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid.ToString().Equals(NodeName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection PreferredDirection)
    {
        // Exact name in the preferred direction first, then any direction, then the display label
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == PreferredDirection && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    // Resolves "NodeName.PinName"
    UEdGraphPin* FindPinRef(UEdGraph* Graph, const FString& Ref, EEdGraphPinDirection PreferredDirection, FString& OutError)
    {
        FString NodeName;
        FString PinName;
        if (!Ref.Split(TEXT("."), &NodeName, &PinName))
        {
            OutError = FString::Printf(TEXT("Pin reference '%s' must look like NodeName.PinName"), *Ref);
            return nullptr;
        }
        UEdGraphNode* Node = FindNode(Graph, NodeName);
        if (!Node)
        {
            OutError = FString::Printf(TEXT("Node '%s' not found in graph %s"), *NodeName, *Graph->GetName());
            return nullptr;
        }
        UEdGraphPin* Pin = FindPinOnNode(Node, PinName, PreferredDirection);
        if (!Pin)
        {
            TArray<FString> PinNames;
            for (const UEdGraphPin* Candidate : Node->Pins)
            {
                if (Candidate && !Candidate->bHidden)
                {
                    PinNames.Add(Candidate->PinName.ToString());
                }
            }
            OutError = FString::Printf(TEXT("Pin '%s' not found on %s. Pins: %s"), *PinName, *NodeName, *FString::Join(PinNames, TEXT(", ")));
        }
        return Pin;
    }

    // Accepts "Actor", "AActor", "AActor*", "BP_Enemy", "BP_Enemy_C" or a full /Script or /Game path
    UClass* FindClassByName(FString Name)
    {
        Name.TrimStartAndEndInline();
        Name.RemoveFromEnd(TEXT("*"));
        if (Name.IsEmpty())
        {
            return nullptr;
        }

        if (Name.StartsWith(TEXT("/")))
        {
            if (UClass* Class = LoadObject<UClass>(nullptr, *Name, nullptr, LOAD_NoWarn))
            {
                return Class;
            }
            if (UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(Name))
            {
                return Blueprint->GeneratedClass;
            }
            return nullptr;
        }

        if (UClass* Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst))
        {
            return Class;
        }
        // C++ spelling with prefix: AActor -> Actor, UObject -> Object
        if (Name.Len() > 1 && (Name[0] == TCHAR('A') || Name[0] == TCHAR('U') || Name[0] == TCHAR('I')) && FChar::IsUpper(Name[1]))
        {
            if (UClass* Class = FindFirstObject<UClass>(*Name.Mid(1), EFindFirstObjectOptions::NativeFirst))
            {
                return Class;
            }
        }
        FString BlueprintName = Name;
        BlueprintName.RemoveFromEnd(TEXT("_C"));
        if (UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName))
        {
            return Blueprint->GeneratedClass;
        }
        return nullptr;
    }

    UScriptStruct* FindStructByName(FString Name)
    {
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("/")))
        {
            return LoadObject<UScriptStruct>(nullptr, *Name, nullptr, LOAD_NoWarn);
        }
        if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst))
        {
            return Struct;
        }
        if (Name.Len() > 1 && Name[0] == TCHAR('F'))
        {
            return FindFirstObject<UScriptStruct>(*Name.Mid(1), EFindFirstObjectOptions::NativeFirst);
        }
        return nullptr;
    }

    UEnum* FindEnumByName(FString Name)
    {
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("/")))
        {
            return LoadObject<UEnum>(nullptr, *Name, nullptr, LOAD_NoWarn);
        }
        return FindFirstObject<UEnum>(*Name, EFindFirstObjectOptions::NativeFirst);
    }

    // Returns the text inside Prefix<...> when Type has that form
    bool StripTemplate(const FString& Type, const TCHAR* Prefix, FString& OutInner)
    {
        const FString Open = FString(Prefix) + TEXT("<");
        if (Type.StartsWith(Open, ESearchCase::IgnoreCase) && Type.EndsWith(TEXT(">")))
        {
            OutInner = Type.Mid(Open.Len(), Type.Len() - Open.Len() - 1).TrimStartAndEnd();
            return true;
        }
        return false;
    }

    // Splits "K, V" at the top-level comma (ignores commas nested in <>)
    bool SplitMapArgs(const FString& Inner, FString& OutKey, FString& OutValue)
    {
        int32 Depth = 0;
        for (int32 Index = 0; Index < Inner.Len(); ++Index)
        {
            const TCHAR Char = Inner[Index];
            if (Char == TCHAR('<'))
            {
                ++Depth;
            }
            else if (Char == TCHAR('>'))
            {
                --Depth;
            }
            else if (Char == TCHAR(',') && Depth == 0)
            {
                OutKey = Inner.Left(Index).TrimStartAndEnd();
                OutValue = Inner.Mid(Index + 1).TrimStartAndEnd();
                return true;
            }
        }
        return false;
    }

    bool ParseBasicType(const FString& Lower, FName& OutCategory, FName& OutSubCategory)
    {
        if (Lower == TEXT("bool") || Lower == TEXT("boolean")) { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
        if (Lower == TEXT("int") || Lower == TEXT("int32") || Lower == TEXT("integer")) { OutCategory = UEdGraphSchema_K2::PC_Int; return true; }
        if (Lower == TEXT("int64")) { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }
        if (Lower == TEXT("float")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Float; return true; }
        if (Lower == TEXT("double") || Lower == TEXT("real")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Double; return true; }
        if (Lower == TEXT("byte") || Lower == TEXT("uint8")) { OutCategory = UEdGraphSchema_K2::PC_Byte; return true; }
        if (Lower == TEXT("name") || Lower == TEXT("fname")) { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
        if (Lower == TEXT("string") || Lower == TEXT("fstring")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
        if (Lower == TEXT("text") || Lower == TEXT("ftext")) { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }
        return false;
    }

    bool ParseTerminalType(FString Type, FName& OutCategory, FName& OutSubCategory, UObject*& OutSubObject, FString& OutError)
    {
        Type.TrimStartAndEndInline();
        OutSubCategory = NAME_None;
        OutSubObject = nullptr;

        if (ParseBasicType(Type.ToLower(), OutCategory, OutSubCategory))
        {
            return true;
        }

        struct FWrapper
        {
            const TCHAR* Prefix;
            FName Category;
        };
        const FWrapper Wrappers[] = {
            { TEXT("TSubclassOf"), UEdGraphSchema_K2::PC_Class },
            { TEXT("TSoftClassPtr"), UEdGraphSchema_K2::PC_SoftClass },
            { TEXT("TSoftObjectPtr"), UEdGraphSchema_K2::PC_SoftObject },
            { TEXT("TScriptInterface"), UEdGraphSchema_K2::PC_Interface },
            { TEXT("TObjectPtr"), UEdGraphSchema_K2::PC_Object },
        };
        for (const FWrapper& Wrapper : Wrappers)
        {
            FString Inner;
            if (StripTemplate(Type, Wrapper.Prefix, Inner))
            {
                UClass* Class = FindClassByName(Inner);
                if (!Class)
                {
                    OutError = FString::Printf(TEXT("Unknown class '%s' in type '%s'"), *Inner, *Type);
                    return false;
                }
                OutCategory = Wrapper.Category;
                OutSubObject = Class;
                return true;
            }
        }

        if (Type.EndsWith(TEXT("*")))
        {
            if (UClass* Class = FindClassByName(Type))
            {
                OutCategory = Class->HasAnyClassFlags(CLASS_Interface) ? UEdGraphSchema_K2::PC_Interface : UEdGraphSchema_K2::PC_Object;
                OutSubObject = Class;
                return true;
            }
            OutError = FString::Printf(TEXT("Unknown class '%s'"), *Type);
            return false;
        }

        if (UScriptStruct* Struct = FindStructByName(Type))
        {
            OutCategory = UEdGraphSchema_K2::PC_Struct;
            OutSubObject = Struct;
            return true;
        }
        if (UEnum* Enum = FindEnumByName(Type))
        {
            OutCategory = UEdGraphSchema_K2::PC_Byte;
            OutSubObject = Enum;
            return true;
        }
        if (UClass* Class = FindClassByName(Type))
        {
            OutCategory = UEdGraphSchema_K2::PC_Object;
            OutSubObject = Class;
            return true;
        }

        OutError = FString::Printf(TEXT("Unknown type '%s'. Use C++ spelling, e.g. bool, int32, float, FVector, AActor*, TSubclassOf<AActor>, TArray<FName>"), *Type);
        return false;
    }

    bool ParsePinType(FString Type, FEdGraphPinType& OutType, FString& OutError)
    {
        Type.TrimStartAndEndInline();
        OutType = FEdGraphPinType();

        FString Inner;
        FName Category;
        FName SubCategory;
        UObject* SubObject = nullptr;

        if (StripTemplate(Type, TEXT("TMap"), Inner))
        {
            FString KeyType;
            FString ValueType;
            if (!SplitMapArgs(Inner, KeyType, ValueType))
            {
                OutError = FString::Printf(TEXT("TMap needs two type arguments: %s"), *Type);
                return false;
            }
            if (!ParseTerminalType(KeyType, Category, SubCategory, SubObject, OutError))
            {
                return false;
            }
            OutType.PinCategory = Category;
            OutType.PinSubCategory = SubCategory;
            OutType.PinSubCategoryObject = SubObject;
            OutType.ContainerType = EPinContainerType::Map;

            if (!ParseTerminalType(ValueType, Category, SubCategory, SubObject, OutError))
            {
                return false;
            }
            OutType.PinValueType.TerminalCategory = Category;
            OutType.PinValueType.TerminalSubCategory = SubCategory;
            OutType.PinValueType.TerminalSubCategoryObject = SubObject;
            return true;
        }

        EPinContainerType Container = EPinContainerType::None;
        if (StripTemplate(Type, TEXT("TArray"), Inner))
        {
            Container = EPinContainerType::Array;
            Type = Inner;
        }
        else if (StripTemplate(Type, TEXT("TSet"), Inner))
        {
            Container = EPinContainerType::Set;
            Type = Inner;
        }

        if (!ParseTerminalType(Type, Category, SubCategory, SubObject, OutError))
        {
            return false;
        }
        OutType.PinCategory = Category;
        OutType.PinSubCategory = SubCategory;
        OutType.PinSubCategoryObject = SubObject;
        OutType.ContainerType = Container;
        return true;
    }

    // Looks a callable function up on Owner, or on the Blueprint itself and the common Blueprint libraries
    UFunction* FindCallableFunction(UBlueprint* Blueprint, const FString& FunctionName, const FString& OwnerName, FString& OutError)
    {
        TArray<UClass*> SearchClasses;
        if (!OwnerName.IsEmpty())
        {
            UClass* OwnerClass = FindClassByName(OwnerName);
            if (!OwnerClass)
            {
                OutError = FString::Printf(TEXT("Unknown class '%s'"), *OwnerName);
                return nullptr;
            }
            SearchClasses.Add(OwnerClass);
        }
        else
        {
            // The skeleton class already knows about functions added since the last compile
            if (Blueprint->SkeletonGeneratedClass)
            {
                SearchClasses.Add(Blueprint->SkeletonGeneratedClass);
            }
            if (Blueprint->GeneratedClass)
            {
                SearchClasses.Add(Blueprint->GeneratedClass);
            }
            const TCHAR* Libraries[] = {
                TEXT("KismetSystemLibrary"), TEXT("KismetMathLibrary"), TEXT("GameplayStatics"),
                TEXT("KismetStringLibrary"), TEXT("KismetTextLibrary"), TEXT("KismetArrayLibrary"),
                TEXT("BlueprintMapLibrary"), TEXT("BlueprintSetLibrary"), TEXT("KismetMaterialLibrary"),
                TEXT("WidgetBlueprintLibrary"), TEXT("AIBlueprintHelperLibrary"), TEXT("KismetInputLibrary")
            };
            for (const TCHAR* Library : Libraries)
            {
                if (UClass* LibraryClass = FindFirstObject<UClass>(Library, EFindFirstObjectOptions::NativeFirst))
                {
                    SearchClasses.Add(LibraryClass);
                }
            }
        }

        for (UClass* Class : SearchClasses)
        {
            // Many engine functions are exposed with a K2_ prefix (K2_GetActorLocation, K2_DestroyActor...)
            if (UFunction* Function = Class->FindFunctionByName(FName(*FunctionName)))
            {
                return Function;
            }
            if (UFunction* Function = Class->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName))))
            {
                return Function;
            }
        }

        OutError = OwnerName.IsEmpty()
            ? FString::Printf(TEXT("Function '%s' not found on the Blueprint or common libraries; pass 'owner' (e.g. Actor, Character, KismetMathLibrary)"), *FunctionName)
            : FString::Printf(TEXT("Function '%s' not found on %s"), *FunctionName, *OwnerName);
        return nullptr;
    }

    TSharedPtr<FJsonObject> CompileAndReport(UBlueprint* Blueprint)
    {
        FCompilerResultsLog Results;
        Results.SetSilentMode(true);
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        FString Status;
        switch (Blueprint->Status)
        {
        case BS_UpToDate: Status = TEXT("up_to_date"); break;
        case BS_UpToDateWithWarnings: Status = TEXT("up_to_date_with_warnings"); break;
        case BS_Error: Status = TEXT("error"); break;
        default: Status = TEXT("dirty"); break;
        }
        Report->SetStringField(TEXT("status"), Status);

        constexpr int32 MaxMessages = 50;
        TArray<TSharedPtr<FJsonValue>> Errors;
        TArray<TSharedPtr<FJsonValue>> Warnings;
        for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
        {
            const EMessageSeverity::Type Severity = Message->GetSeverity();
            if (Severity == EMessageSeverity::Error && Errors.Num() < MaxMessages)
            {
                Errors.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
            }
            else if ((Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning) && Warnings.Num() < MaxMessages)
            {
                Warnings.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
            }
        }
        Report->SetArrayField(TEXT("errors"), Errors);
        Report->SetArrayField(TEXT("warnings"), Warnings);
        return Report;
    }

    bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool bDefault)
    {
        bool bValue = bDefault;
        Params->TryGetBoolField(Name, bValue);
        return bValue;
    }

    TArray<TSharedPtr<FJsonValue>> NodesToJson(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes)
    {
        TMap<const UEdGraphNode*, FString> NodeIds;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                NodeIds.Add(Node, Node->GetName());
            }
        }

        TArray<TSharedPtr<FJsonValue>> Out;
        for (UEdGraphNode* Node : Nodes)
        {
            if (Node && NodeIds.Contains(Node))
            {
                Out.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, NodeIds)));
            }
        }
        return Out;
    }

    TSharedPtr<FJsonObject> MakeResult()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        return Result;
    }

    // Marks the Blueprint changed, refreshes open editors and (unless "compile": false) recompiles and reports
    void FinishEdit(UBlueprint* Blueprint, UEdGraph* Graph, bool bStructural, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Result)
    {
        if (bStructural)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        if (Graph)
        {
            Graph->NotifyGraphChanged();
        }
        if (GetBoolParam(Params, TEXT("compile"), true))
        {
            Result->SetObjectField(TEXT("compile"), CompileAndReport(Blueprint));
        }
    }

    UBlueprint* GetBlueprintParam(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString BlueprintRef;
        if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintRef) && !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
        {
            OutError = TEXT("Missing 'blueprint' parameter");
            return nullptr;
        }
        UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintRef);
        if (!Blueprint)
        {
            OutError = FString::Printf(TEXT("Blueprint not found: %s (use list_blueprints to get its path)"), *BlueprintRef);
        }
        return Blueprint;
    }

    UEdGraph* GetGraphParam(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString GraphName;
        Params->TryGetStringField(TEXT("graph"), GraphName);
        UEdGraph* Graph = FindGraph(Blueprint, GraphName);
        if (!Graph)
        {
            OutError = FString::Printf(TEXT("Graph '%s' not found. Available graphs: %s"),
                GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName, *GraphNamesList(Blueprint));
        }
        return Graph;
    }

    UEdGraph* FindStandardMacro(const FString& MacroName, FString& OutError)
    {
        UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
        if (!Macros)
        {
            OutError = TEXT("Could not load StandardMacros");
            return nullptr;
        }
        TArray<FString> Names;
        for (UEdGraph* Graph : Macros->MacroGraphs)
        {
            if (Graph && Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
            if (Graph)
            {
                Names.Add(Graph->GetName());
            }
        }
        OutError = FString::Printf(TEXT("Macro '%s' not found. Standard macros: %s"), *MacroName, *FString::Join(Names, TEXT(", ")));
        return nullptr;
    }

    // Creates the (not yet placed) node object described by Spec; returns an existing node for duplicate event overrides
    UEdGraphNode* ConstructNodeFromSpec(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& Spec, bool& bOutExisting, FString& OutError)
    {
        bOutExisting = false;
        auto GetString = [&Spec](const TCHAR* Key)
        {
            FString Value;
            Spec->TryGetStringField(Key, Value);
            return Value;
        };
        const FString Type = GetString(TEXT("type")).ToLower();
        const FString Owner = GetString(TEXT("owner"));

        if (Type == TEXT("call_function") || Type == TEXT("function"))
        {
            UFunction* Function = FindCallableFunction(Blueprint, GetString(TEXT("function")), Owner, OutError);
            if (!Function)
            {
                return nullptr;
            }
            // Array library functions need the wildcard-aware array node
            UK2Node_CallFunction* CallNode = nullptr;
            if (Function->GetOwnerClass()->IsChildOf(UKismetArrayLibrary::StaticClass()))
            {
                CallNode = NewObject<UK2Node_CallArrayFunction>(Graph);
            }
            else
            {
                CallNode = NewObject<UK2Node_CallFunction>(Graph);
            }
            CallNode->SetFromFunction(Function);
            return CallNode;
        }

        if (Type == TEXT("variable_get") || Type == TEXT("variable_set"))
        {
            const FString VariableName = GetString(TEXT("variable"));
            if (VariableName.IsEmpty())
            {
                OutError = TEXT("variable_get/variable_set need 'variable'");
                return nullptr;
            }
            UK2Node_Variable* VariableNode = nullptr;
            if (Type == TEXT("variable_get"))
            {
                VariableNode = NewObject<UK2Node_VariableGet>(Graph);
            }
            else
            {
                VariableNode = NewObject<UK2Node_VariableSet>(Graph);
            }
            if (Owner.IsEmpty())
            {
                VariableNode->VariableReference.SetSelfMember(FName(*VariableName));
            }
            else
            {
                UClass* OwnerClass = FindClassByName(Owner);
                if (!OwnerClass)
                {
                    OutError = FString::Printf(TEXT("Unknown class '%s'"), *Owner);
                    return nullptr;
                }
                VariableNode->VariableReference.SetExternalMember(FName(*VariableName), OwnerClass);
            }
            return VariableNode;
        }

        if (Type == TEXT("event"))
        {
            const FString EventName = GetString(TEXT("event"));
            UClass* SearchClass = Owner.IsEmpty() ? Blueprint->ParentClass.Get() : FindClassByName(Owner);
            UFunction* Function = SearchClass ? SearchClass->FindFunctionByName(FName(*EventName)) : nullptr;
            if (!Function && SearchClass)
            {
                // BeginPlay -> ReceiveBeginPlay, Tick -> ReceiveTick ...
                Function = SearchClass->FindFunctionByName(FName(*(TEXT("Receive") + EventName)));
            }
            if (!Function)
            {
                OutError = FString::Printf(TEXT("Overridable event '%s' not found on %s"), *EventName, SearchClass ? *SearchClass->GetName() : TEXT("parent class"));
                return nullptr;
            }
            UClass* DeclaringClass = Function->GetOwnerClass();
            if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, DeclaringClass, Function->GetFName()))
            {
                bOutExisting = true;
                return Existing;
            }
            UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
            EventNode->EventReference.SetExternalMember(Function->GetFName(), DeclaringClass);
            EventNode->bOverrideFunction = true;
            return EventNode;
        }

        if (Type == TEXT("custom_event"))
        {
            UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
            const FString Name = GetString(TEXT("name"));
            EventNode->CustomFunctionName = Name.IsEmpty() ? FBlueprintEditorUtils::FindUniqueCustomEventName(Blueprint) : FName(*Name);
            return EventNode;
        }

        if (Type == TEXT("branch"))
        {
            return NewObject<UK2Node_IfThenElse>(Graph);
        }
        if (Type == TEXT("sequence"))
        {
            return NewObject<UK2Node_ExecutionSequence>(Graph);
        }
        if (Type == TEXT("self"))
        {
            return NewObject<UK2Node_Self>(Graph);
        }
        if (Type == TEXT("reroute"))
        {
            return NewObject<UK2Node_Knot>(Graph);
        }

        if (Type == TEXT("cast"))
        {
            UClass* TargetClass = FindClassByName(GetString(TEXT("class")));
            if (!TargetClass)
            {
                OutError = FString::Printf(TEXT("cast needs a valid 'class' (got '%s')"), *GetString(TEXT("class")));
                return nullptr;
            }
            UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
            CastNode->TargetType = TargetClass;
            return CastNode;
        }

        if (Type == TEXT("macro"))
        {
            UEdGraph* MacroGraph = FindStandardMacro(GetString(TEXT("macro")), OutError);
            if (!MacroGraph)
            {
                return nullptr;
            }
            UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
            MacroNode->SetMacroGraph(MacroGraph);
            return MacroNode;
        }

        if (Type == TEXT("make_struct") || Type == TEXT("break_struct"))
        {
            UScriptStruct* Struct = FindStructByName(GetString(TEXT("struct")));
            if (!Struct)
            {
                OutError = FString::Printf(TEXT("%s needs a valid 'struct' (got '%s')"), *Type, *GetString(TEXT("struct")));
                return nullptr;
            }
            if (Type == TEXT("make_struct"))
            {
                UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph);
                MakeNode->StructType = Struct;
                return MakeNode;
            }
            UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph);
            BreakNode->StructType = Struct;
            return BreakNode;
        }

        if (Type == TEXT("node"))
        {
            // Any graph node class, configured through "properties"
            const FString ClassName = GetString(TEXT("class"));
            UClass* NodeClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
            if (!NodeClass)
            {
                NodeClass = FindFirstObject<UClass>(*(TEXT("K2Node_") + ClassName), EFindFirstObjectOptions::NativeFirst);
            }
            if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
            {
                OutError = FString::Printf(TEXT("'%s' is not a placeable graph node class"), *ClassName);
                return nullptr;
            }
            return NewObject<UEdGraphNode>(Graph, NodeClass);
        }

        OutError = FString::Printf(TEXT("Unknown node type '%s'. Use call_function, variable_get, variable_set, event, custom_event, branch, sequence, self, reroute, cast, macro, make_struct, break_struct or node"), *Type);
        return nullptr;
    }

    // Builds, configures and places a node; returns the node (or an existing event override)
    UEdGraphNode* CreateNodeFromSpec(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& Spec, bool& bOutExisting, FString& OutError)
    {
        UEdGraphNode* Node = ConstructNodeFromSpec(Blueprint, Graph, Spec, bOutExisting, OutError);
        if (!Node || bOutExisting)
        {
            return Node;
        }

        // Optional raw property values in UE text format, e.g. {"properties": {"NumOutputs": "3"}}
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (Spec->TryGetObjectField(TEXT("properties"), Properties))
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
            {
                FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), *Pair.Key);
                if (!Property)
                {
                    OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Pair.Key, *Node->GetClass()->GetName());
                    Node->MarkAsGarbage();
                    return nullptr;
                }
                Property->ImportText_InContainer(*Pair.Value->AsString(), Node, Node, PPF_None);
            }
        }

        double X = 0.0;
        double Y = 0.0;
        Spec->TryGetNumberField(TEXT("x"), X);
        Spec->TryGetNumberField(TEXT("y"), Y);
        Node->NodePosX = static_cast<int32>(X);
        Node->NodePosY = static_cast<int32>(Y);

        Node->SetFlags(RF_Transactional);
        Graph->AddNode(Node, false, false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        if (Node->Pins.Num() == 0)
        {
            Node->AllocateDefaultPins();
        }
        return Node;
    }

    // Node sizes are only known to the UI, so estimate them from the title and visible pins
    FIntPoint EstimateNodeSize(const UEdGraphNode* Node)
    {
        if (Node->NodeWidth > 0 && Node->NodeHeight > 0)
        {
            return FIntPoint(Node->NodeWidth, Node->NodeHeight);
        }
        int32 Inputs = 0;
        int32 Outputs = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && !Pin->bHidden)
            {
                (Pin->Direction == EGPD_Input ? Inputs : Outputs)++;
            }
        }
        const int32 TitleLength = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Len();
        const int32 Width = FMath::Clamp(TitleLength * 8 + 80, 180, 460);
        const int32 Height = 56 + 26 * FMath::Max(Inputs, Outputs);
        return FIntPoint(Width, Height);
    }

    bool HasExecPin(const UEdGraphNode* Node)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                return true;
            }
        }
        return false;
    }

    bool HasIncomingExec(const UEdGraphNode* Node, const TSet<UEdGraphNode*>& NodeSet)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (Linked && NodeSet.Contains(Linked->GetOwningNode()))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Assigns each node a column (Layer) and a horizontal band (Lane)
    void ComputeLayout(const TArray<UEdGraphNode*>& Nodes, TMap<UEdGraphNode*, int32>& Layer, TMap<UEdGraphNode*, int32>& Lane, int32& OutLaneCount)
    {
        const TSet<UEdGraphNode*> NodeSet(Nodes);
        const int32 MaxLayer = Nodes.Num();
        OutLaneCount = 0;

        // Every exec chain that starts at an event (no incoming exec) gets its own lane, kept in the current vertical order
        TArray<UEdGraphNode*> Roots;
        for (UEdGraphNode* Node : Nodes)
        {
            if (HasExecPin(Node) && !HasIncomingExec(Node, NodeSet))
            {
                Roots.Add(Node);
            }
        }
        Roots.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosY < B.NodePosY; });

        for (UEdGraphNode* Root : Roots)
        {
            if (Lane.Contains(Root))
            {
                continue;
            }
            const int32 CurrentLane = OutLaneCount++;
            Layer.Add(Root, 0);
            Lane.Add(Root, CurrentLane);

            // Longest-path layering along exec wires; the layer cap stops exec loops from running forever
            TArray<UEdGraphNode*> Queue;
            Queue.Add(Root);
            for (int32 Index = 0; Index < Queue.Num(); ++Index)
            {
                UEdGraphNode* Node = Queue[Index];
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                    {
                        continue;
                    }
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                        if (!Next || !NodeSet.Contains(Next))
                        {
                            continue;
                        }
                        const int32 NewLayer = Layer[Node] + 1;
                        const int32* ExistingLane = Lane.Find(Next);
                        if (NewLayer > MaxLayer || (ExistingLane && *ExistingLane != CurrentLane))
                        {
                            continue;
                        }
                        const int32* ExistingLayer = Layer.Find(Next);
                        if (!ExistingLayer || *ExistingLayer < NewLayer)
                        {
                            Layer.Add(Next, NewLayer);
                            Lane.Add(Next, CurrentLane);
                            Queue.Add(Next);
                        }
                    }
                }
            }
        }

        // Pure (data) nodes sit one column left of the closest node that consumes their output
        for (int32 Iteration = 0; Iteration < Nodes.Num(); ++Iteration)
        {
            bool bChanged = false;
            for (UEdGraphNode* Node : Nodes)
            {
                if (HasExecPin(Node) && Layer.Contains(Node))
                {
                    continue;
                }
                int32 BestLayer = MAX_int32;
                int32 BestLane = INDEX_NONE;
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Output)
                    {
                        continue;
                    }
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Consumer = Linked ? Linked->GetOwningNode() : nullptr;
                        const int32* ConsumerLayer = Consumer ? Layer.Find(Consumer) : nullptr;
                        if (ConsumerLayer && *ConsumerLayer - 1 < BestLayer)
                        {
                            BestLayer = *ConsumerLayer - 1;
                            BestLane = Lane[Consumer];
                        }
                    }
                }
                const int32* CurrentLayer = Layer.Find(Node);
                if (BestLane != INDEX_NONE && BestLayer >= -MaxLayer && (!CurrentLayer || *CurrentLayer != BestLayer))
                {
                    Layer.Add(Node, BestLayer);
                    Lane.Add(Node, BestLane);
                    bChanged = true;
                }
            }
            if (!bChanged)
            {
                break;
            }
        }

        // Anything left (unconnected nodes) goes into one extra lane
        bool bHasLeftovers = false;
        for (UEdGraphNode* Node : Nodes)
        {
            if (!Layer.Contains(Node))
            {
                Layer.Add(Node, 0);
                Lane.Add(Node, OutLaneCount);
                bHasLeftovers = true;
            }
        }
        if (bHasLeftovers)
        {
            ++OutLaneCount;
        }
    }

    void AutoLayoutNodes(const TArray<UEdGraphNode*>& Nodes)
    {
        if (Nodes.Num() == 0)
        {
            return;
        }

        TMap<UEdGraphNode*, int32> Layer;
        TMap<UEdGraphNode*, int32> Lane;
        int32 LaneCount = 0;
        ComputeLayout(Nodes, Layer, Lane, LaneCount);

        // Keep the laid-out block where the nodes were
        int32 OriginX = MAX_int32;
        int32 OriginY = MAX_int32;
        int32 MinLayer = MAX_int32;
        int32 MaxLayer = MIN_int32;
        TMap<int32, int32> ColumnWidth;
        for (UEdGraphNode* Node : Nodes)
        {
            OriginX = FMath::Min(OriginX, Node->NodePosX);
            OriginY = FMath::Min(OriginY, Node->NodePosY);
            const int32 NodeLayer = Layer[Node];
            MinLayer = FMath::Min(MinLayer, NodeLayer);
            MaxLayer = FMath::Max(MaxLayer, NodeLayer);
            int32& Width = ColumnWidth.FindOrAdd(NodeLayer);
            Width = FMath::Max(Width, EstimateNodeSize(Node).X);
        }

        constexpr int32 ColumnGap = 80;
        constexpr int32 VerticalGap = 40;
        constexpr int32 LaneGap = 160;

        TMap<int32, int32> ColumnX;
        int32 X = OriginX;
        for (int32 Column = MinLayer; Column <= MaxLayer; ++Column)
        {
            ColumnX.Add(Column, X);
            if (const int32* Width = ColumnWidth.Find(Column))
            {
                X += *Width + ColumnGap;
            }
        }

        int32 LaneTop = OriginY;
        for (int32 LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
        {
            TMap<int32, TArray<UEdGraphNode*>> Columns;
            for (UEdGraphNode* Node : Nodes)
            {
                if (Lane[Node] == LaneIndex)
                {
                    Columns.FindOrAdd(Layer[Node]).Add(Node);
                }
            }

            int32 LaneHeight = 0;
            for (TPair<int32, TArray<UEdGraphNode*>>& Column : Columns)
            {
                // Exec nodes on top, then keep the previous vertical order
                Column.Value.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
                {
                    const bool bAExec = HasExecPin(&A);
                    const bool bBExec = HasExecPin(&B);
                    return bAExec != bBExec ? bAExec : A.NodePosY < B.NodePosY;
                });

                int32 Y = LaneTop;
                for (UEdGraphNode* Node : Column.Value)
                {
                    Node->Modify();
                    Node->NodePosX = ColumnX[Column.Key];
                    Node->NodePosY = Y;
                    Y += EstimateNodeSize(Node).Y + VerticalGap;
                }
                LaneHeight = FMath::Max(LaneHeight, Y - LaneTop);
            }
            LaneTop += LaneHeight + LaneGap;
        }
    }
}

using namespace UnrealMCPBlueprintEdit;

FUnrealMCPBlueprintEditCommands::FUnrealMCPBlueprintEditCommands()
{
}

const TMap<FString, FUnrealMCPBlueprintEditCommands::FHandler>& FUnrealMCPBlueprintEditCommands::GetHandlers()
{
    static const TMap<FString, FHandler> Handlers = {
        { TEXT("add_blueprint_nodes"), &FUnrealMCPBlueprintEditCommands::HandleAddNodes },
        { TEXT("paste_blueprint_nodes"), &FUnrealMCPBlueprintEditCommands::HandlePasteNodes },
        { TEXT("delete_blueprint_nodes"), &FUnrealMCPBlueprintEditCommands::HandleDeleteNodes },
        { TEXT("connect_blueprint_pins"), &FUnrealMCPBlueprintEditCommands::HandleConnectPins },
        { TEXT("disconnect_blueprint_pins"), &FUnrealMCPBlueprintEditCommands::HandleDisconnectPins },
        { TEXT("set_blueprint_pin_defaults"), &FUnrealMCPBlueprintEditCommands::HandleSetPinDefaults },
        { TEXT("move_blueprint_nodes"), &FUnrealMCPBlueprintEditCommands::HandleMoveNodes },
        { TEXT("add_blueprint_comment"), &FUnrealMCPBlueprintEditCommands::HandleAddComment },
        { TEXT("auto_layout_blueprint_graph"), &FUnrealMCPBlueprintEditCommands::HandleAutoLayout },
        { TEXT("add_blueprint_function"), &FUnrealMCPBlueprintEditCommands::HandleAddFunction },
        { TEXT("remove_blueprint_graph"), &FUnrealMCPBlueprintEditCommands::HandleRemoveGraph },
        { TEXT("rename_blueprint_graph"), &FUnrealMCPBlueprintEditCommands::HandleRenameGraph },
        { TEXT("add_blueprint_variables"), &FUnrealMCPBlueprintEditCommands::HandleAddVariables },
        { TEXT("remove_blueprint_variable"), &FUnrealMCPBlueprintEditCommands::HandleRemoveVariable },
        { TEXT("modify_blueprint_variable"), &FUnrealMCPBlueprintEditCommands::HandleModifyVariable },
        { TEXT("remove_blueprint_component"), &FUnrealMCPBlueprintEditCommands::HandleRemoveComponent },
        { TEXT("compile_blueprint_report"), &FUnrealMCPBlueprintEditCommands::HandleCompile },
        { TEXT("save_blueprint"), &FUnrealMCPBlueprintEditCommands::HandleSave },
    };
    return Handlers;
}

const TArray<FString>& FUnrealMCPBlueprintEditCommands::GetCommandNames()
{
    static TArray<FString> Names;
    if (Names.Num() == 0)
    {
        GetHandlers().GetKeys(Names);
    }
    return Names;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (const FHandler* Handler = GetHandlers().Find(CommandType))
    {
        return (this->**Handler)(Params);
    }
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleAddNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
    if (!Graph || !Params->TryGetArrayField(TEXT("nodes"), Specs))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'nodes' array") : Error);
    }

    TArray<UEdGraphNode*> Created;
    TArray<TSharedPtr<FJsonValue>> Errors;
    bool bStructural = false;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Blueprint Nodes")));
        Blueprint->Modify();
        Graph->Modify();

        for (int32 Index = 0; Index < Specs->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* Spec = nullptr;
            if (!(*Specs)[Index]->TryGetObject(Spec))
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("nodes[%d] is not an object"), Index)));
                continue;
            }
            bool bExisting = false;
            FString NodeError;
            UEdGraphNode* Node = CreateNodeFromSpec(Blueprint, Graph, *Spec, bExisting, NodeError);
            if (!Node)
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("nodes[%d]: %s"), Index, *NodeError)));
                continue;
            }
            Created.Add(Node);
            if (const UK2Node* K2Node = Cast<UK2Node>(Node))
            {
                bStructural |= K2Node->NodeCausesStructuralBlueprintChange();
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetArrayField(TEXT("nodes"), NodesToJson(Graph, Created));
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    FinishEdit(Blueprint, Graph, bStructural, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandlePasteNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    FString Text;
    if (!Graph || !Params->TryGetStringField(TEXT("t3d"), Text))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 't3d' text") : Error);
    }
    if (!FEdGraphUtilities::CanImportNodesFromText(Graph, Text))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("The text contains no nodes that can be pasted into this graph"));
    }

    TSet<UEdGraphNode*> Pasted;
    bool bStructural = false;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Paste Blueprint Nodes")));
        Blueprint->Modify();
        Graph->Modify();
        FEdGraphUtilities::ImportNodesFromText(Graph, Text, Pasted);

        // Optionally move the pasted block so its top-left corner lands on x/y
        double TargetX = 0.0;
        double TargetY = 0.0;
        const bool bPlace = Params->TryGetNumberField(TEXT("x"), TargetX) && Params->TryGetNumberField(TEXT("y"), TargetY);
        int32 MinX = MAX_int32;
        int32 MinY = MAX_int32;
        for (const UEdGraphNode* Node : Pasted)
        {
            MinX = FMath::Min(MinX, Node->NodePosX);
            MinY = FMath::Min(MinY, Node->NodePosY);
        }

        for (UEdGraphNode* Node : Pasted)
        {
            // Pasted copies must not share GUIDs with the originals
            Node->CreateNewGuid();
            if (bPlace)
            {
                Node->NodePosX += static_cast<int32>(TargetX) - MinX;
                Node->NodePosY += static_cast<int32>(TargetY) - MinY;
            }
            if (const UK2Node* K2Node = Cast<UK2Node>(Node))
            {
                bStructural |= K2Node->NodeCausesStructuralBlueprintChange();
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetArrayField(TEXT("nodes"), NodesToJson(Graph, Pasted.Array()));
    FinishEdit(Blueprint, Graph, bStructural, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleDeleteNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    TArray<FString> NodeNames;
    if (!Graph || !Params->TryGetStringArrayField(TEXT("nodes"), NodeNames))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'nodes' array of node names") : Error);
    }

    TArray<TSharedPtr<FJsonValue>> Deleted;
    TArray<TSharedPtr<FJsonValue>> Errors;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Delete Blueprint Nodes")));
        Blueprint->Modify();
        Graph->Modify();
        for (const FString& NodeName : NodeNames)
        {
            UEdGraphNode* Node = FindNode(Graph, NodeName);
            if (!Node)
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Node '%s' not found"), *NodeName)));
                continue;
            }
            if (!Node->CanUserDeleteNode())
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Node '%s' cannot be deleted"), *NodeName)));
                continue;
            }
            Node->Modify();
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            Deleted.Add(MakeShared<FJsonValueString>(NodeName));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetArrayField(TEXT("deleted"), Deleted);
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    FinishEdit(Blueprint, Graph, true, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleConnectPins(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
    if (!Graph || !Params->TryGetArrayField(TEXT("connections"), Connections))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'connections' array of {from, to}") : Error);
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    int32 Connected = 0;
    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Notes;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Connect Blueprint Pins")));
        Graph->Modify();
        for (const TSharedPtr<FJsonValue>& Value : *Connections)
        {
            const TSharedPtr<FJsonObject>* Connection = nullptr;
            FString From;
            FString To;
            if (!Value->TryGetObject(Connection) || !(*Connection)->TryGetStringField(TEXT("from"), From) || !(*Connection)->TryGetStringField(TEXT("to"), To))
            {
                Errors.Add(MakeShared<FJsonValueString>(TEXT("Each connection needs 'from' and 'to' (NodeName.PinName)")));
                continue;
            }
            FString PinError;
            UEdGraphPin* FromPin = FindPinRef(Graph, From, EGPD_Output, PinError);
            UEdGraphPin* ToPin = FromPin ? FindPinRef(Graph, To, EGPD_Input, PinError) : nullptr;
            if (!FromPin || !ToPin)
            {
                Errors.Add(MakeShared<FJsonValueString>(PinError));
                continue;
            }

            const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
            if (Response.Response == CONNECT_RESPONSE_DISALLOW)
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s -> %s: %s"), *From, *To, *Response.Message.ToString())));
                continue;
            }
            if (Schema->TryCreateConnection(FromPin, ToPin))
            {
                ++Connected;
                if (Response.Response != CONNECT_RESPONSE_MAKE && !Response.Message.IsEmpty())
                {
                    Notes.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s -> %s: %s"), *From, *To, *Response.Message.ToString())));
                }
            }
            else
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s -> %s: connection failed"), *From, *To)));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetNumberField(TEXT("connected"), Connected);
    if (Notes.Num() > 0)
    {
        Result->SetArrayField(TEXT("notes"), Notes);
    }
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleDisconnectPins(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    int32 Broken = 0;
    TArray<TSharedPtr<FJsonValue>> Errors;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Disconnect Blueprint Pins")));
        Graph->Modify();

        // Specific wires: [{from, to}]
        const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
        if (Params->TryGetArrayField(TEXT("connections"), Connections))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Connections)
            {
                const TSharedPtr<FJsonObject>* Connection = nullptr;
                FString From;
                FString To;
                if (!Value->TryGetObject(Connection) || !(*Connection)->TryGetStringField(TEXT("from"), From) || !(*Connection)->TryGetStringField(TEXT("to"), To))
                {
                    Errors.Add(MakeShared<FJsonValueString>(TEXT("Each connection needs 'from' and 'to'")));
                    continue;
                }
                FString PinError;
                UEdGraphPin* FromPin = FindPinRef(Graph, From, EGPD_Output, PinError);
                UEdGraphPin* ToPin = FromPin ? FindPinRef(Graph, To, EGPD_Input, PinError) : nullptr;
                if (!FromPin || !ToPin)
                {
                    Errors.Add(MakeShared<FJsonValueString>(PinError));
                    continue;
                }
                if (!FromPin->LinkedTo.Contains(ToPin))
                {
                    Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s and %s are not connected"), *From, *To)));
                    continue;
                }
                Schema->BreakSinglePinLink(FromPin, ToPin);
                ++Broken;
            }
        }

        // Every wire on a pin: ["Node.Pin"]
        TArray<FString> Pins;
        if (Params->TryGetStringArrayField(TEXT("pins"), Pins))
        {
            for (const FString& Ref : Pins)
            {
                FString PinError;
                UEdGraphPin* Pin = FindPinRef(Graph, Ref, EGPD_Input, PinError);
                if (!Pin)
                {
                    Errors.Add(MakeShared<FJsonValueString>(PinError));
                    continue;
                }
                Broken += Pin->LinkedTo.Num();
                Schema->BreakPinLinks(*Pin, true);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetNumberField(TEXT("disconnected"), Broken);
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleSetPinDefaults(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Graph || !Params->TryGetArrayField(TEXT("values"), Values))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'values' array of {pin, value}") : Error);
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Errors;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Blueprint Pin Defaults")));
        Graph->Modify();
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString Ref;
            FString NewValue;
            if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("pin"), Ref) || !(*Entry)->TryGetStringField(TEXT("value"), NewValue))
            {
                Errors.Add(MakeShared<FJsonValueString>(TEXT("Each entry needs 'pin' (NodeName.PinName) and 'value'")));
                continue;
            }
            FString PinError;
            UEdGraphPin* Pin = FindPinRef(Graph, Ref, EGPD_Input, PinError);
            if (!Pin)
            {
                Errors.Add(MakeShared<FJsonValueString>(PinError));
                continue;
            }
            if (Pin->Direction != EGPD_Input)
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s is an output pin"), *Ref)));
                continue;
            }

            Pin->GetOwningNode()->Modify();
            const FName Category = Pin->PinType.PinCategory;
            const bool bObjectPin = Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
                || Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
                || Category == UEdGraphSchema_K2::PC_Interface;
            if (bObjectPin)
            {
                // Object pins take an asset or class path, or an empty string to clear
                UObject* Object = NewValue.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *NewValue, nullptr, LOAD_NoWarn);
                if (!NewValue.IsEmpty() && !Object)
                {
                    Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: object '%s' not found"), *Ref, *NewValue)));
                    continue;
                }
                Schema->TrySetDefaultObject(*Pin, Object);
            }
            else
            {
                Schema->TrySetDefaultValue(*Pin, NewValue);
            }

            TSharedPtr<FJsonObject> AppliedJson = MakeShared<FJsonObject>();
            AppliedJson->SetStringField(TEXT("pin"), Ref);
            AppliedJson->SetStringField(TEXT("value"), Pin->GetDefaultAsString());
            if (Pin->LinkedTo.Num() > 0)
            {
                AppliedJson->SetStringField(TEXT("note"), TEXT("pin is connected, so the default is ignored"));
            }
            Applied.Add(MakeShared<FJsonValueObject>(AppliedJson));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetArrayField(TEXT("applied"), Applied);
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleMoveNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Moves = nullptr;
    if (!Graph || !Params->TryGetArrayField(TEXT("moves"), Moves))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'moves' array of {node, x, y}") : Error);
    }

    int32 Moved = 0;
    TArray<TSharedPtr<FJsonValue>> Errors;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Move Blueprint Nodes")));
        for (const TSharedPtr<FJsonValue>& Value : *Moves)
        {
            const TSharedPtr<FJsonObject>* Move = nullptr;
            FString NodeName;
            double X = 0.0;
            double Y = 0.0;
            if (!Value->TryGetObject(Move) || !(*Move)->TryGetStringField(TEXT("node"), NodeName)
                || !(*Move)->TryGetNumberField(TEXT("x"), X) || !(*Move)->TryGetNumberField(TEXT("y"), Y))
            {
                Errors.Add(MakeShared<FJsonValueString>(TEXT("Each move needs 'node', 'x' and 'y'")));
                continue;
            }
            UEdGraphNode* Node = FindNode(Graph, NodeName);
            if (!Node)
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Node '%s' not found"), *NodeName)));
                continue;
            }
            Node->Modify();
            Node->NodePosX = static_cast<int32>(X);
            Node->NodePosY = static_cast<int32>(Y);
            ++Moved;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetNumberField(TEXT("moved"), Moved);
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    // Layout never changes behaviour, so skip the recompile unless asked
    if (!Params->HasField(TEXT("compile")))
    {
        Params->SetBoolField(TEXT("compile"), false);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleAddComment(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    FString Text;
    if (!Graph || !Params->TryGetStringField(TEXT("text"), Text))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'text'") : Error);
    }

    // Either wrap the given nodes or use an explicit rectangle
    double X = 0.0;
    double Y = 0.0;
    double Width = 400.0;
    double Height = 200.0;
    TArray<FString> NodeNames;
    if (Params->TryGetStringArrayField(TEXT("nodes"), NodeNames) && NodeNames.Num() > 0)
    {
        int32 MinX = MAX_int32;
        int32 MinY = MAX_int32;
        int32 MaxX = MIN_int32;
        int32 MaxY = MIN_int32;
        for (const FString& NodeName : NodeNames)
        {
            UEdGraphNode* Node = FindNode(Graph, NodeName);
            if (!Node)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node '%s' not found"), *NodeName));
            }
            const FIntPoint Size = EstimateNodeSize(Node);
            MinX = FMath::Min(MinX, Node->NodePosX);
            MinY = FMath::Min(MinY, Node->NodePosY);
            MaxX = FMath::Max(MaxX, Node->NodePosX + Size.X);
            MaxY = FMath::Max(MaxY, Node->NodePosY + Size.Y);
        }
        constexpr int32 Padding = 40;
        constexpr int32 TitleHeight = 60;
        X = MinX - Padding;
        Y = MinY - Padding - TitleHeight;
        Width = (MaxX - MinX) + Padding * 2;
        Height = (MaxY - MinY) + Padding * 2 + TitleHeight;
    }
    else
    {
        Params->TryGetNumberField(TEXT("x"), X);
        Params->TryGetNumberField(TEXT("y"), Y);
        Params->TryGetNumberField(TEXT("width"), Width);
        Params->TryGetNumberField(TEXT("height"), Height);
    }

    UEdGraphNode_Comment* Comment = nullptr;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Blueprint Comment")));
        Graph->Modify();
        Comment = NewObject<UEdGraphNode_Comment>(Graph);
        Comment->SetFlags(RF_Transactional);
        Graph->AddNode(Comment, false, false);
        Comment->CreateNewGuid();
        Comment->PostPlacedNewNode();
        Comment->NodeComment = Text;
        Comment->NodePosX = static_cast<int32>(X);
        Comment->NodePosY = static_cast<int32>(Y);
        Comment->NodeWidth = static_cast<int32>(Width);
        Comment->NodeHeight = static_cast<int32>(Height);

        const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
        if (Params->TryGetArrayField(TEXT("color"), Color) && Color->Num() >= 3)
        {
            Comment->CommentColor = FLinearColor(
                static_cast<float>((*Color)[0]->AsNumber()),
                static_cast<float>((*Color)[1]->AsNumber()),
                static_cast<float>((*Color)[2]->AsNumber()),
                Color->Num() > 3 ? static_cast<float>((*Color)[3]->AsNumber()) : 1.0f);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("node"), Comment->GetName());
    if (!Params->HasField(TEXT("compile")))
    {
        Params->SetBoolField(TEXT("compile"), false);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleAutoLayout(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    // Lay out the given nodes, or every node except comment boxes
    TArray<UEdGraphNode*> Nodes;
    TArray<FString> NodeNames;
    if (Params->TryGetStringArrayField(TEXT("nodes"), NodeNames) && NodeNames.Num() > 0)
    {
        for (const FString& NodeName : NodeNames)
        {
            if (UEdGraphNode* Node = FindNode(Graph, NodeName))
            {
                Nodes.Add(Node);
            }
        }
    }
    else
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && !Node->IsA<UEdGraphNode_Comment>())
            {
                Nodes.Add(Node);
            }
        }
    }

    bool bHasComments = false;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        bHasComments |= Node && Node->IsA<UEdGraphNode_Comment>();
    }

    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Auto Layout Blueprint Graph")));
        Graph->Modify();
        AutoLayoutNodes(Nodes);
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetNumberField(TEXT("arranged"), Nodes.Num());
    if (bHasComments)
    {
        Result->SetStringField(TEXT("note"), TEXT("Comment boxes are not moved; delete and re-add them around the new node positions if needed"));
    }
    if (!Params->HasField(TEXT("compile")))
    {
        Params->SetBoolField(TEXT("compile"), false);
    }
    FinishEdit(Blueprint, Graph, false, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleAddFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    FString FunctionName;
    if (!Blueprint || !Params->TryGetStringField(TEXT("name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'name'") : Error);
    }
    if (FindGraph(Blueprint, FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("A graph named '%s' already exists"), *FunctionName));
    }

    // Validate every parameter type before touching the Blueprint
    struct FParam
    {
        FName Name;
        FEdGraphPinType Type;
    };
    auto ParseParams = [](const TSharedPtr<FJsonObject>& Source, const TCHAR* Field, TArray<FParam>& Out, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Source->TryGetArrayField(Field, Values))
        {
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString Name;
            FString Type;
            if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("name"), Name) || !(*Entry)->TryGetStringField(TEXT("type"), Type))
            {
                OutError = FString::Printf(TEXT("Each entry in '%s' needs 'name' and 'type'"), Field);
                return false;
            }
            FParam Param;
            Param.Name = FName(*Name);
            if (!ParsePinType(Type, Param.Type, OutError))
            {
                return false;
            }
            Out.Add(Param);
        }
        return true;
    };
    TArray<FParam> Inputs;
    TArray<FParam> Outputs;
    if (!ParseParams(Params, TEXT("inputs"), Inputs, Error) || !ParseParams(Params, TEXT("outputs"), Outputs, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    UEdGraph* Graph = nullptr;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Blueprint Function")));
        Blueprint->Modify();
        Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);

        UK2Node_FunctionEntry* Entry = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
            {
                Entry = Candidate;
                break;
            }
        }
        if (Entry)
        {
            // Function inputs are outputs of the entry node; return values are inputs of the result node
            for (const FParam& Input : Inputs)
            {
                Entry->CreateUserDefinedPin(Input.Name, Input.Type, EGPD_Output);
            }
            if (Outputs.Num() > 0)
            {
                if (UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry))
                {
                    for (const FParam& Output : Outputs)
                    {
                        ResultNode->CreateUserDefinedPin(Output.Name, Output.Type, EGPD_Input);
                    }
                }
            }

            if (GetBoolParam(Params, TEXT("pure"), false))
            {
                Entry->AddExtraFlags(FUNC_BlueprintPure);
            }
            if (GetBoolParam(Params, TEXT("const"), false))
            {
                Entry->AddExtraFlags(FUNC_Const);
            }
            FString Access;
            if (Params->TryGetStringField(TEXT("access"), Access))
            {
                Entry->ClearExtraFlags(FUNC_AccessSpecifiers);
                Entry->AddExtraFlags(Access.Equals(TEXT("private"), ESearchCase::IgnoreCase) ? FUNC_Private
                    : Access.Equals(TEXT("protected"), ESearchCase::IgnoreCase) ? FUNC_Protected : FUNC_Public);
            }
            FString Category;
            if (Params->TryGetStringField(TEXT("category"), Category))
            {
                Entry->MetaData.Category = FText::FromString(Category);
            }
            FString Tooltip;
            if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
            {
                Entry->MetaData.ToolTip = FText::FromString(Tooltip);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    TArray<UEdGraphNode*> FunctionNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        FunctionNodes.Add(Node);
    }
    Result->SetArrayField(TEXT("nodes"), NodesToJson(Graph, FunctionNodes));
    FinishEdit(Blueprint, Graph, true, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleRemoveGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    if (Blueprint->UbergraphPages.Num() <= 1 && Blueprint->UbergraphPages.Contains(Graph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Refusing to remove the only event graph"));
    }

    const FString GraphName = Graph->GetName();
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Blueprint Graph")));
        Blueprint->Modify();
        FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("removed"), GraphName);
    FinishEdit(Blueprint, nullptr, true, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleRenameGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    UEdGraph* Graph = Blueprint ? GetGraphParam(Blueprint, Params, Error) : nullptr;
    FString NewName;
    if (!Graph || !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'new_name'") : Error);
    }
    if (FindGraph(Blueprint, NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("A graph named '%s' already exists"), *NewName));
    }

    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Rename Blueprint Graph")));
        Blueprint->Modify();
        Graph->Modify();
        FBlueprintEditorUtils::RenameGraph(Graph, NewName);
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    FinishEdit(Blueprint, Graph, true, Params, Result);
    return Result;
}

namespace UnrealMCPBlueprintEdit
{
    void ApplyVariableOptions(UBlueprint* Blueprint, const FName VarName, const TSharedPtr<FJsonObject>& Spec)
    {
        FString Category;
        if (Spec->TryGetStringField(TEXT("category"), Category))
        {
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(Category), true);
        }
        bool bInstanceEditable = false;
        if (Spec->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
        {
            FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bInstanceEditable);
        }
        FString Tooltip;
        if (Spec->TryGetStringField(TEXT("tooltip"), Tooltip))
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
        }
        bool bExposeOnSpawn = false;
        if (Spec->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn))
        {
            if (bExposeOnSpawn)
            {
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
            }
            else
            {
                FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
            }
        }
    }

    // Writes a default value (UE text format, e.g. "1.5", "True", "(X=1,Y=2,Z=3)") into the compiled class defaults
    bool SetVariableDefault(UBlueprint* Blueprint, const FName VarName, const FString& Value, FString& OutError)
    {
        const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
        if (Index != INDEX_NONE)
        {
            Blueprint->NewVariables[Index].DefaultValue = Value;
        }

        UClass* GeneratedClass = Blueprint->GeneratedClass;
        UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
        FProperty* Property = GeneratedClass ? FindFProperty<FProperty>(GeneratedClass, VarName) : nullptr;
        if (!CDO || !Property)
        {
            OutError = FString::Printf(TEXT("Variable '%s' is not on the compiled class yet; compile the Blueprint and set the default again"), *VarName.ToString());
            return false;
        }
        CDO->Modify();
        if (!Property->ImportText_InContainer(*Value, CDO, CDO, PPF_None))
        {
            OutError = FString::Printf(TEXT("Could not parse '%s' as a value for %s"), *Value, *VarName.ToString());
            return false;
        }
        CDO->MarkPackageDirty();
        return true;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleAddVariables(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
    if (!Blueprint || !Params->TryGetArrayField(TEXT("variables"), Specs))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'variables' array of {name, type, ...}") : Error);
    }

    TArray<TSharedPtr<FJsonValue>> Added;
    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TPair<FName, FString>> Defaults;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Blueprint Variables")));
        Blueprint->Modify();
        for (const TSharedPtr<FJsonValue>& Value : *Specs)
        {
            const TSharedPtr<FJsonObject>* Spec = nullptr;
            FString Name;
            FString TypeString;
            if (!Value->TryGetObject(Spec) || !(*Spec)->TryGetStringField(TEXT("name"), Name) || !(*Spec)->TryGetStringField(TEXT("type"), TypeString))
            {
                Errors.Add(MakeShared<FJsonValueString>(TEXT("Each variable needs 'name' and 'type'")));
                continue;
            }
            FEdGraphPinType PinType;
            FString TypeError;
            if (!ParsePinType(TypeString, PinType, TypeError))
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Name, *TypeError)));
                continue;
            }
            FString DefaultValue;
            (*Spec)->TryGetStringField(TEXT("default"), DefaultValue);
            if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Name), PinType, DefaultValue))
            {
                Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: could not add (name already used?)"), *Name)));
                continue;
            }
            ApplyVariableOptions(Blueprint, FName(*Name), *Spec);
            if (!DefaultValue.IsEmpty())
            {
                Defaults.Emplace(FName(*Name), DefaultValue);
            }
            Added.Add(MakeShared<FJsonValueString>(Name));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetArrayField(TEXT("added"), Added);
    FinishEdit(Blueprint, nullptr, true, Params, Result);

    // Defaults go into the class defaults, which only exist after the compile above
    for (const TPair<FName, FString>& Default : Defaults)
    {
        FString DefaultError;
        if (!SetVariableDefault(Blueprint, Default.Key, Default.Value, DefaultError))
        {
            Errors.Add(MakeShared<FJsonValueString>(DefaultError));
        }
    }
    if (Errors.Num() > 0)
    {
        Result->SetArrayField(TEXT("errors"), Errors);
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleRemoveVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    FString Name;
    if (!Blueprint || !Params->TryGetStringField(TEXT("name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'name'") : Error);
    }
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Variable '%s' not found"), *Name));
    }

    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Blueprint Variable")));
        Blueprint->Modify();
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Name));
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("removed"), Name);
    FinishEdit(Blueprint, nullptr, true, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleModifyVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    FString Name;
    if (!Blueprint || !Params->TryGetStringField(TEXT("name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'name'") : Error);
    }
    FName VarName(*Name);
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Variable '%s' not found"), *Name));
    }

    FEdGraphPinType NewType;
    FString TypeString;
    const bool bChangeType = Params->TryGetStringField(TEXT("type"), TypeString);
    if (bChangeType && !ParsePinType(TypeString, NewType, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Modify Blueprint Variable")));
        Blueprint->Modify();
        if (bChangeType)
        {
            FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VarName, NewType);
        }
        ApplyVariableOptions(Blueprint, VarName, Params);

        FString NewName;
        if (Params->TryGetStringField(TEXT("new_name"), NewName) && !NewName.IsEmpty() && NewName != Name)
        {
            FBlueprintEditorUtils::RenameMemberVariable(Blueprint, VarName, FName(*NewName));
            VarName = FName(*NewName);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("name"), VarName.ToString());
    FinishEdit(Blueprint, nullptr, true, Params, Result);

    FString DefaultValue;
    if (Params->TryGetStringField(TEXT("default"), DefaultValue))
    {
        FString DefaultError;
        if (!SetVariableDefault(Blueprint, VarName, DefaultValue, DefaultError))
        {
            Result->SetStringField(TEXT("default_error"), DefaultError);
        }
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    FString Name;
    if (!Blueprint || !Params->TryGetStringField(TEXT("name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("Missing 'name'") : Error);
    }
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* Target = nullptr;
    if (SCS)
    {
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (Node && Node->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                Target = Node;
                break;
            }
        }
    }
    if (!Target)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component '%s' not found (only components added in this Blueprint can be removed)"), *Name));
    }

    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Blueprint Component")));
        Blueprint->Modify();
        SCS->Modify();
        SCS->RemoveNodeAndPromoteChildren(Target);
    }

    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("removed"), Name);
    FinishEdit(Blueprint, nullptr, true, Params, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleCompile(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetObjectField(TEXT("compile"), CompileAndReport(Blueprint));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintEditCommands::HandleSave(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UBlueprint* Blueprint = GetBlueprintParam(Params, Error);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    if (!UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Saving %s failed"), *Blueprint->GetPathName()));
    }
    TSharedPtr<FJsonObject> Result = MakeResult();
    Result->SetStringField(TEXT("saved"), Blueprint->GetPathName());
    return Result;
}
