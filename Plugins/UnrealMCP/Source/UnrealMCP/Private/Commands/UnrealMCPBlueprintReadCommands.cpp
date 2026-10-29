#include "Commands/UnrealMCPBlueprintReadCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/TimelineTemplate.h"
#include "Components/ActorComponent.h"
#include "Curves/CurveFloat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "UnrealMCPBlueprintGraphJson.h"

namespace UnrealMCPBlueprintRead
{
    // Long property values (big arrays, structs) are cut to keep responses readable
    constexpr int32 MaxValueLength = 1000;

    FString Truncate(const FString& In)
    {
        return In.Len() > MaxValueLength ? In.Left(MaxValueLength) + TEXT("...(truncated)") : In;
    }

    // C++ spelling of a single (non-container) pin type, independent of the editor language
    FString TerminalTypeToCpp(const FName Category, const FName SubCategory, const UObject* SubCategoryObject)
    {
        const UClass* Class = Cast<UClass>(SubCategoryObject);
        const FString ClassName = Class ? FString(Class->GetPrefixCPP()) + Class->GetName() : TEXT("UObject");

        if (Category == UEdGraphSchema_K2::PC_Exec) return TEXT("exec");
        if (Category == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
        if (Category == UEdGraphSchema_K2::PC_Int) return TEXT("int32");
        if (Category == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
        if (Category == UEdGraphSchema_K2::PC_Float) return TEXT("float");
        if (Category == UEdGraphSchema_K2::PC_Double) return TEXT("double");
        if (Category == UEdGraphSchema_K2::PC_Real) return SubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
        if (Category == UEdGraphSchema_K2::PC_Name) return TEXT("FName");
        if (Category == UEdGraphSchema_K2::PC_String) return TEXT("FString");
        if (Category == UEdGraphSchema_K2::PC_Text) return TEXT("FText");
        if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
        {
            const UEnum* Enum = Cast<UEnum>(SubCategoryObject);
            return Enum ? Enum->GetName() : TEXT("uint8");
        }
        if (Category == UEdGraphSchema_K2::PC_Struct)
        {
            const UScriptStruct* Struct = Cast<UScriptStruct>(SubCategoryObject);
            return Struct ? Struct->GetStructCPPName() : TEXT("struct");
        }
        if (Category == UEdGraphSchema_K2::PC_Object)
        {
            return SubCategory == UEdGraphSchema_K2::PSC_Self ? TEXT("self") : ClassName + TEXT("*");
        }
        if (Category == UEdGraphSchema_K2::PC_Class)
        {
            return SubCategory == UEdGraphSchema_K2::PSC_Self ? TEXT("TSubclassOf<self>") : TEXT("TSubclassOf<") + ClassName + TEXT(">");
        }
        if (Category == UEdGraphSchema_K2::PC_SoftObject) return TEXT("TSoftObjectPtr<") + ClassName + TEXT(">");
        if (Category == UEdGraphSchema_K2::PC_SoftClass) return TEXT("TSoftClassPtr<") + ClassName + TEXT(">");
        if (Category == UEdGraphSchema_K2::PC_Interface) return TEXT("TScriptInterface<") + ClassName + TEXT(">");
        if (Category == UEdGraphSchema_K2::PC_Delegate || Category == UEdGraphSchema_K2::PC_MCDelegate)
        {
            const UFunction* Signature = Cast<UFunction>(SubCategoryObject);
            return Signature ? TEXT("delegate:") + Signature->GetName() : TEXT("delegate");
        }
        if (Category == UEdGraphSchema_K2::PC_Wildcard) return TEXT("wildcard");
        return Category.ToString();
    }

    FString PinTypeToString(const FEdGraphPinType& Type)
    {
        const FString Terminal = TerminalTypeToCpp(Type.PinCategory, Type.PinSubCategory, Type.PinSubCategoryObject.Get());
        FString Result;
        switch (Type.ContainerType)
        {
        case EPinContainerType::Array:
            Result = TEXT("TArray<") + Terminal + TEXT(">");
            break;
        case EPinContainerType::Set:
            Result = TEXT("TSet<") + Terminal + TEXT(">");
            break;
        case EPinContainerType::Map:
            Result = TEXT("TMap<") + Terminal + TEXT(", ") + TerminalTypeToCpp(Type.PinValueType.TerminalCategory,
                Type.PinValueType.TerminalSubCategory, Type.PinValueType.TerminalSubCategoryObject.Get()) + TEXT(">");
            break;
        default:
            Result = Terminal;
            break;
        }
        if (Type.bIsReference)
        {
            Result += TEXT("&");
        }
        return Result;
    }

    FString SingleLine(FString In)
    {
        In.ReplaceInline(TEXT("\r"), TEXT(""));
        In.ReplaceInline(TEXT("\n"), TEXT(" | "));
        return In;
    }

    void AddString(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Value)
    {
        Array.Add(MakeShared<FJsonValueString>(Value));
    }

    /**
     * Exports the editable properties of Obj whose value differs from Baseline.
     * Properties Baseline doesn't have are exported when set; properties owned by SkipOwner are ignored.
     */
    TSharedPtr<FJsonObject> DiffEditableProperties(UObject* Obj, UObject* Baseline, const UClass* SkipOwner = nullptr)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Obj)
        {
            return Out;
        }

        for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }
            if (SkipOwner && Prop->GetOwnerClass() == SkipOwner)
            {
                continue;
            }

            const bool bHasBaseline = Baseline && Baseline->GetClass()->IsChildOf(Prop->GetOwnerClass());
            if (bHasBaseline && Prop->Identical_InContainer(Obj, Baseline))
            {
                continue;
            }

            if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
            {
                UObject* Value = ObjProp->GetObjectPropertyValue_InContainer(Obj);
                // Instanced subobjects (components etc.) always differ between archetypes
                if (Value && Value->IsIn(Obj))
                {
                    continue;
                }
                if (!bHasBaseline && !Value)
                {
                    continue;
                }
            }

            FString ValueStr;
            Prop->ExportTextItem_InContainer(ValueStr, Obj, nullptr, Obj, PPF_None);
            Out->SetStringField(Prop->GetName(), Truncate(ValueStr));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> VariablesToJson(const TArray<FBPVariableDescription>& Variables, UClass* OwnerClass, UObject* CDO)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FBPVariableDescription& Var : Variables)
        {
            TSharedPtr<FJsonObject> VarJson = MakeShared<FJsonObject>();
            VarJson->SetStringField(TEXT("name"), Var.VarName.ToString());
            VarJson->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));

            const FString Category = Var.Category.ToString();
            if (!Category.IsEmpty() && Category != TEXT("Default"))
            {
                VarJson->SetStringField(TEXT("category"), Category);
            }

            // Prefer the compiled CDO value; fall back to the stored default string
            FString DefaultValue = Var.DefaultValue;
            if (OwnerClass && CDO)
            {
                if (FProperty* Prop = FindFProperty<FProperty>(OwnerClass, Var.VarName))
                {
                    DefaultValue.Reset();
                    Prop->ExportTextItem_InContainer(DefaultValue, CDO, nullptr, CDO, PPF_None);
                }
            }
            if (!DefaultValue.IsEmpty())
            {
                VarJson->SetStringField(TEXT("default"), Truncate(DefaultValue));
            }

            TArray<TSharedPtr<FJsonValue>> Flags;
            if ((Var.PropertyFlags & CPF_Edit) && !(Var.PropertyFlags & CPF_DisableEditOnInstance))
            {
                AddString(Flags, TEXT("InstanceEditable"));
            }
            if (Var.PropertyFlags & CPF_BlueprintReadOnly)
            {
                AddString(Flags, TEXT("BlueprintReadOnly"));
            }
            if (Var.PropertyFlags & CPF_Net)
            {
                AddString(Flags, TEXT("Replicated"));
            }
            if (Var.PropertyFlags & CPF_Transient)
            {
                AddString(Flags, TEXT("Transient"));
            }
            if (Var.PropertyFlags & CPF_SaveGame)
            {
                AddString(Flags, TEXT("SaveGame"));
            }
            if (Var.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn))
            {
                AddString(Flags, TEXT("ExposeOnSpawn"));
            }
            if (Flags.Num() > 0)
            {
                VarJson->SetArrayField(TEXT("flags"), Flags);
            }
            if (!Var.RepNotifyFunc.IsNone())
            {
                VarJson->SetStringField(TEXT("rep_notify"), Var.RepNotifyFunc.ToString());
            }
            if (Var.HasMetaData(FBlueprintMetadata::MD_Tooltip))
            {
                VarJson->SetStringField(TEXT("tooltip"), Var.GetMetaData(FBlueprintMetadata::MD_Tooltip));
            }
            Out.Add(MakeShared<FJsonValueObject>(VarJson));
        }
        return Out;
    }

    TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
        PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinJson->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));

        PinJson->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));

        if (!Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString() != Pin->PinName.ToString())
        {
            PinJson->SetStringField(TEXT("label"), Pin->PinFriendlyName.ToString());
        }

        if (Pin->LinkedTo.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Links;
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked)
                {
                    continue;
                }
                const FString* LinkedId = NodeIds.Find(Linked->GetOwningNode());
                const FString LinkedNode = LinkedId ? *LinkedId : FString(TEXT("unknown"));
                AddString(Links, LinkedNode + TEXT(".") + Linked->PinName.ToString());
            }
            PinJson->SetArrayField(TEXT("links"), Links);
        }
        else if (Pin->Direction == EGPD_Input)
        {
            const FString DefaultValue = Pin->GetDefaultAsString();
            if (!DefaultValue.IsEmpty())
            {
                PinJson->SetStringField(TEXT("default"), Truncate(DefaultValue));
            }
        }
        return PinJson;
    }

    TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
        NodeJson->SetStringField(TEXT("id"), NodeIds.FindChecked(Node));

        FString ClassName = Node->GetClass()->GetName();
        ClassName.RemoveFromStart(TEXT("K2Node_"));
        NodeJson->SetStringField(TEXT("class"), ClassName);
        NodeJson->SetStringField(TEXT("title"), SingleLine(Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()));

        // Comment boxes carry their text in the title
        if (Node->IsA<UEdGraphNode_Comment>())
        {
            return NodeJson;
        }

        if (!Node->NodeComment.IsEmpty())
        {
            NodeJson->SetStringField(TEXT("comment"), SingleLine(Node->NodeComment));
        }
        if (!Node->IsNodeEnabled())
        {
            NodeJson->SetBoolField(TEXT("disabled"), true);
        }

        if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
        {
            NodeJson->SetStringField(TEXT("function"), CallNode->FunctionReference.GetMemberName().ToString());
            if (UFunction* Function = CallNode->GetTargetFunction())
            {
                if (UClass* OwnerClass = Function->GetOwnerClass())
                {
                    NodeJson->SetStringField(TEXT("function_owner"), OwnerClass->GetName());
                }
                if (Function->HasMetaData(TEXT("Latent")))
                {
                    NodeJson->SetBoolField(TEXT("latent"), true);
                }
            }
            if (CallNode->IsNodePure())
            {
                NodeJson->SetBoolField(TEXT("pure"), true);
            }
        }
        else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
        {
            NodeJson->SetStringField(TEXT("variable"), VarNode->GetVarName().ToString());
            if (!VarNode->VariableReference.IsSelfContext())
            {
                if (UClass* VarOwner = VarNode->VariableReference.GetMemberParentClass())
                {
                    NodeJson->SetStringField(TEXT("variable_owner"), VarOwner->GetName());
                }
            }
        }
        else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
        {
            NodeJson->SetStringField(TEXT("event"), CustomEventNode->CustomFunctionName.ToString());
            NodeJson->SetBoolField(TEXT("custom"), true);
        }
        else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
        {
            NodeJson->SetStringField(TEXT("event"), EventNode->EventReference.GetMemberName().ToString());
        }
        else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
        {
            if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
            {
                NodeJson->SetStringField(TEXT("macro"), MacroGraph->GetName());
            }
        }
        else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            if (CastNode->TargetType)
            {
                NodeJson->SetStringField(TEXT("cast_to"), CastNode->TargetType->GetName());
            }
        }
        else if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
        {
            NodeJson->SetStringField(TEXT("timeline"), TimelineNode->TimelineName.ToString());
        }

        // Entry/result/event nodes define a signature, so their unconnected outputs still matter
        const bool bSignatureNode = Node->IsA<UK2Node_EditablePinBase>();

        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Pin->LinkedTo.Num() == 0)
            {
                const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                if (Pin->bHidden
                    || (Pin->Direction == EGPD_Output && !bSignatureNode)
                    || (Pin->Direction == EGPD_Input && bExec))
                {
                    continue;
                }
            }
            Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
        }
        NodeJson->SetArrayField(TEXT("pins"), Pins);
        return NodeJson;
    }

    TSharedPtr<FJsonObject> FunctionInfoToJson(const UK2Node_FunctionEntry* Entry)
    {
        TSharedPtr<FJsonObject> InfoJson = MakeShared<FJsonObject>();
        const int32 Flags = Entry->GetFunctionFlags();

        FString Access = TEXT("public");
        if (Flags & FUNC_Protected)
        {
            Access = TEXT("protected");
        }
        else if (Flags & FUNC_Private)
        {
            Access = TEXT("private");
        }
        InfoJson->SetStringField(TEXT("access"), Access);
        if (Flags & FUNC_BlueprintPure)
        {
            InfoJson->SetBoolField(TEXT("pure"), true);
        }
        if (Flags & FUNC_Const)
        {
            InfoJson->SetBoolField(TEXT("const"), true);
        }
        if (!Entry->MetaData.ToolTip.IsEmpty())
        {
            InfoJson->SetStringField(TEXT("tooltip"), Entry->MetaData.ToolTip.ToString());
        }
        if (!Entry->MetaData.Category.IsEmpty())
        {
            InfoJson->SetStringField(TEXT("category"), Entry->MetaData.Category.ToString());
        }
        if (Entry->LocalVariables.Num() > 0)
        {
            InfoJson->SetArrayField(TEXT("local_variables"), VariablesToJson(Entry->LocalVariables, nullptr, nullptr));
        }
        return InfoJson;
    }

    TSharedPtr<FJsonObject> GraphToJson(UEdGraph* Graph, const FString& Kind, bool bIncludeNodes, bool bIncludeJson, bool bIncludeT3D)
    {
        TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
        GraphJson->SetStringField(TEXT("name"), Graph->GetName());
        GraphJson->SetStringField(TEXT("kind"), Kind);
        GraphJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                GraphJson->SetObjectField(TEXT("function_info"), FunctionInfoToJson(Entry));
                break;
            }
        }

        if (!bIncludeNodes)
        {
            return GraphJson;
        }

        if (bIncludeJson)
        {
            // Node object names stay stable when other nodes are added or removed, so edit commands can use them
            TMap<const UEdGraphNode*, FString> NodeIds;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    NodeIds.Add(Node, Node->GetName());
                }
            }

            TArray<TSharedPtr<FJsonValue>> Nodes;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, NodeIds)));
                }
            }
            GraphJson->SetArrayField(TEXT("nodes"), Nodes);
        }

        if (bIncludeT3D)
        {
            // Same text the editor puts on the clipboard for Ctrl+C
            TSet<UObject*> NodesToExport;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    NodesToExport.Add(Node);
                }
            }
            FString ExportedText;
            FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
            GraphJson->SetStringField(TEXT("t3d"), ExportedText);
        }
        return GraphJson;
    }

    void AddScsNode(USCS_Node* Node, const FString& ParentName, TArray<TSharedPtr<FJsonValue>>& Out)
    {
        if (!Node)
        {
            return;
        }

        const FString Name = Node->GetVariableName().ToString();
        TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
        ComponentJson->SetStringField(TEXT("name"), Name);
        ComponentJson->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("None"));
        if (!ParentName.IsEmpty())
        {
            ComponentJson->SetStringField(TEXT("parent"), ParentName);
        }
        if (!Node->AttachToName.IsNone())
        {
            ComponentJson->SetStringField(TEXT("socket"), Node->AttachToName.ToString());
        }
        if (Node->ComponentTemplate && Node->ComponentClass)
        {
            TSharedPtr<FJsonObject> Overrides = DiffEditableProperties(Node->ComponentTemplate, Node->ComponentClass->GetDefaultObject());
            if (Overrides->Values.Num() > 0)
            {
                ComponentJson->SetObjectField(TEXT("properties"), Overrides);
            }
        }
        Out.Add(MakeShared<FJsonValueObject>(ComponentJson));

        for (USCS_Node* Child : Node->GetChildNodes())
        {
            AddScsNode(Child, Name, Out);
        }
    }

    TArray<TSharedPtr<FJsonValue>> InheritedComponentOverrides(UBlueprint* Blueprint, UObject* CDO, UObject* SuperCDO)
    {
        TArray<TSharedPtr<FJsonValue>> Out;

        // Components created by C++ parents live as default subobjects of the CDO
        if (CDO)
        {
            TArray<UObject*> Subobjects;
            CDO->GetDefaultSubobjects(Subobjects);
            for (UObject* Subobject : Subobjects)
            {
                if (!Subobject || !Subobject->IsA<UActorComponent>())
                {
                    continue;
                }
                UObject* Baseline = SuperCDO ? SuperCDO->GetDefaultSubobjectByName(Subobject->GetFName()) : nullptr;
                if (!Baseline)
                {
                    Baseline = Subobject->GetClass()->GetDefaultObject();
                }
                TSharedPtr<FJsonObject> Overrides = DiffEditableProperties(Subobject, Baseline);
                if (Overrides->Values.Num() > 0)
                {
                    TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
                    ComponentJson->SetStringField(TEXT("name"), Subobject->GetName());
                    ComponentJson->SetStringField(TEXT("class"), Subobject->GetClass()->GetName());
                    ComponentJson->SetStringField(TEXT("source"), TEXT("native_parent"));
                    ComponentJson->SetObjectField(TEXT("properties"), Overrides);
                    Out.Add(MakeShared<FJsonValueObject>(ComponentJson));
                }
            }
        }

        // Components added by Blueprint parents are overridden through the inheritable component handler
        if (Blueprint->InheritableComponentHandler)
        {
            TArray<UActorComponent*> Templates;
            Blueprint->InheritableComponentHandler->GetAllTemplates(Templates);
            for (UActorComponent* Template : Templates)
            {
                if (!Template)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Overrides = DiffEditableProperties(Template, Template->GetArchetype());
                if (Overrides->Values.Num() > 0)
                {
                    FString Name = Template->GetName();
                    Name.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
                    TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
                    ComponentJson->SetStringField(TEXT("name"), Name);
                    ComponentJson->SetStringField(TEXT("class"), Template->GetClass()->GetName());
                    ComponentJson->SetStringField(TEXT("source"), TEXT("blueprint_parent"));
                    ComponentJson->SetObjectField(TEXT("properties"), Overrides);
                    Out.Add(MakeShared<FJsonValueObject>(ComponentJson));
                }
            }
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> CurveKeysToJson(const UCurveFloat* Curve)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        if (!Curve)
        {
            return Keys;
        }
        for (const FRichCurveKey& Key : Curve->FloatCurve.Keys)
        {
            TArray<TSharedPtr<FJsonValue>> Pair;
            Pair.Add(MakeShared<FJsonValueNumber>(Key.Time));
            Pair.Add(MakeShared<FJsonValueNumber>(Key.Value));
            Keys.Add(MakeShared<FJsonValueArray>(Pair));
        }
        return Keys;
    }

    TArray<TSharedPtr<FJsonValue>> TimelinesToJson(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const UTimelineTemplate* Timeline : Blueprint->Timelines)
        {
            if (!Timeline)
            {
                continue;
            }
            TSharedPtr<FJsonObject> TimelineJson = MakeShared<FJsonObject>();
            TimelineJson->SetStringField(TEXT("name"), Timeline->GetVariableName().ToString());
            TimelineJson->SetNumberField(TEXT("length"), Timeline->TimelineLength);
            TimelineJson->SetBoolField(TEXT("auto_play"), Timeline->bAutoPlay != 0);
            TimelineJson->SetBoolField(TEXT("loop"), Timeline->bLoop != 0);

            TArray<TSharedPtr<FJsonValue>> FloatTracks;
            for (const FTTFloatTrack& Track : Timeline->FloatTracks)
            {
                TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
                TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
                TrackJson->SetArrayField(TEXT("keys"), CurveKeysToJson(Track.CurveFloat));
                FloatTracks.Add(MakeShared<FJsonValueObject>(TrackJson));
            }
            TimelineJson->SetArrayField(TEXT("float_tracks"), FloatTracks);

            TArray<TSharedPtr<FJsonValue>> EventTracks;
            for (const FTTEventTrack& Track : Timeline->EventTracks)
            {
                TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
                TrackJson->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
                TrackJson->SetArrayField(TEXT("keys"), CurveKeysToJson(Track.CurveKeys));
                EventTracks.Add(MakeShared<FJsonValueObject>(TrackJson));
            }
            TimelineJson->SetArrayField(TEXT("event_tracks"), EventTracks);

            TArray<TSharedPtr<FJsonValue>> OtherTracks;
            for (const FTTVectorTrack& Track : Timeline->VectorTracks)
            {
                AddString(OtherTracks, TEXT("vector:") + Track.GetTrackName().ToString());
            }
            for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
            {
                AddString(OtherTracks, TEXT("color:") + Track.GetTrackName().ToString());
            }
            if (OtherTracks.Num() > 0)
            {
                TimelineJson->SetArrayField(TEXT("other_tracks"), OtherTracks);
            }
            Out.Add(MakeShared<FJsonValueObject>(TimelineJson));
        }
        return Out;
    }

    TSharedPtr<FJsonObject> WidgetToJson(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> WidgetJson = MakeShared<FJsonObject>();
        WidgetJson->SetStringField(TEXT("name"), Widget->GetName());
        WidgetJson->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        if (Widget->bIsVariable)
        {
            WidgetJson->SetBoolField(TEXT("is_variable"), true);
        }
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            TArray<TSharedPtr<FJsonValue>> Children;
            for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
            {
                if (UWidget* Child = Panel->GetChildAt(Index))
                {
                    Children.Add(MakeShared<FJsonValueObject>(WidgetToJson(Child)));
                }
            }
            WidgetJson->SetArrayField(TEXT("children"), Children);
        }
        return WidgetJson;
    }
}

using namespace UnrealMCPBlueprintRead;

FUnrealMCPBlueprintReadCommands::FUnrealMCPBlueprintReadCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintReadCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("list_blueprints"))
    {
        return HandleListBlueprints(Params);
    }
    else if (CommandType == TEXT("read_blueprint"))
    {
        return HandleReadBlueprint(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint read command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintReadCommands::HandleListBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    Path.RemoveFromEnd(TEXT("/"));

    FString NameFilter;
    Params->TryGetStringField(TEXT("name_filter"), NameFilter);

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*Path));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

    constexpr int32 MaxResults = 500;
    bool bTruncated = false;
    TArray<TSharedPtr<FJsonValue>> Blueprints;
    for (const FAssetData& Asset : Assets)
    {
        if (!NameFilter.IsEmpty() && !Asset.AssetName.ToString().Contains(NameFilter))
        {
            continue;
        }
        if (Blueprints.Num() >= MaxResults)
        {
            bTruncated = true;
            break;
        }

        TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
        AssetJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        AssetJson->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        AssetJson->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
        FString ParentClass;
        if (Asset.GetTagValue(FName(TEXT("ParentClass")), ParentClass))
        {
            AssetJson->SetStringField(TEXT("parent_class"), ParentClass);
        }
        Blueprints.Add(MakeShared<FJsonValueObject>(AssetJson));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("count"), Blueprints.Num());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetArrayField(TEXT("blueprints"), Blueprints);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintReadCommands::HandleReadBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintRef) && !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintRef);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s (use list_blueprints to get its path)"), *BlueprintRef));
    }

    FString GraphFilter;
    Params->TryGetStringField(TEXT("graph"), GraphFilter);

    bool bIncludeGraphs = true;
    Params->TryGetBoolField(TEXT("include_graphs"), bIncludeGraphs);

    FString Format = TEXT("json");
    Params->TryGetStringField(TEXT("format"), Format);
    Format.ToLowerInline();
    const bool bIncludeT3D = Format == TEXT("t3d") || Format == TEXT("both");
    const bool bIncludeJson = Format != TEXT("t3d");

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("name"), Blueprint->GetName());
    Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("asset_class"), Blueprint->GetClass()->GetName());
    Result->SetStringField(TEXT("blueprint_type"), StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(Blueprint->BlueprintType)));
    Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("None"));

    FString Status;
    switch (Blueprint->Status)
    {
    case BS_UpToDate: Status = TEXT("up_to_date"); break;
    case BS_UpToDateWithWarnings: Status = TEXT("up_to_date_with_warnings"); break;
    case BS_Dirty: Status = TEXT("dirty"); break;
    case BS_Error: Status = TEXT("error"); break;
    default: Status = TEXT("unknown"); break;
    }
    Result->SetStringField(TEXT("compile_status"), Status);

    // A single-graph request only returns that graph, to keep follow-up reads small
    if (GraphFilter.IsEmpty())
    {
        UClass* GeneratedClass = Blueprint->GeneratedClass;
        UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject(false) : nullptr;
        UClass* SuperClass = GeneratedClass ? GeneratedClass->GetSuperClass() : nullptr;
        UObject* SuperCDO = SuperClass ? SuperClass->GetDefaultObject(false) : nullptr;

        TArray<TSharedPtr<FJsonValue>> Interfaces;
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            if (Interface.Interface)
            {
                AddString(Interfaces, Interface.Interface->GetName());
            }
        }
        if (Interfaces.Num() > 0)
        {
            Result->SetArrayField(TEXT("interfaces"), Interfaces);
        }

        Result->SetArrayField(TEXT("variables"), VariablesToJson(Blueprint->NewVariables, GeneratedClass, CDO));

        if (Blueprint->SimpleConstructionScript)
        {
            TArray<TSharedPtr<FJsonValue>> Components;
            for (USCS_Node* RootNode : Blueprint->SimpleConstructionScript->GetRootNodes())
            {
                const FString ParentName = RootNode && !RootNode->ParentComponentOrVariableName.IsNone()
                    ? RootNode->ParentComponentOrVariableName.ToString()
                    : FString();
                AddScsNode(RootNode, ParentName, Components);
            }
            Result->SetArrayField(TEXT("components"), Components);
        }

        TArray<TSharedPtr<FJsonValue>> InheritedOverrides = InheritedComponentOverrides(Blueprint, CDO, SuperCDO);
        if (InheritedOverrides.Num() > 0)
        {
            Result->SetArrayField(TEXT("inherited_component_overrides"), InheritedOverrides);
        }

        if (CDO)
        {
            TSharedPtr<FJsonObject> ClassDefaults = DiffEditableProperties(CDO, SuperCDO, GeneratedClass);
            if (ClassDefaults->Values.Num() > 0)
            {
                Result->SetObjectField(TEXT("class_defaults"), ClassDefaults);
            }
        }

        if (Blueprint->Timelines.Num() > 0)
        {
            Result->SetArrayField(TEXT("timelines"), TimelinesToJson(Blueprint));
        }

        if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
        {
            if (WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget)
            {
                Result->SetObjectField(TEXT("widget_tree"), WidgetToJson(WidgetBlueprint->WidgetTree->RootWidget));
            }
        }
    }

    TArray<TPair<UEdGraph*, FString>> AllGraphs;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        AllGraphs.Emplace(Graph, TEXT("event"));
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        AllGraphs.Emplace(Graph, TEXT("function"));
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        AllGraphs.Emplace(Graph, TEXT("macro"));
    }
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        AllGraphs.Emplace(Graph, TEXT("dispatcher"));
    }
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        for (UEdGraph* Graph : Interface.Graphs)
        {
            AllGraphs.Emplace(Graph, TEXT("interface"));
        }
    }

    TArray<TSharedPtr<FJsonValue>> Graphs;
    TArray<FString> GraphNames;
    for (const TPair<UEdGraph*, FString>& Entry : AllGraphs)
    {
        UEdGraph* Graph = Entry.Key;
        if (!Graph)
        {
            continue;
        }
        GraphNames.Add(Graph->GetName());
        if (!GraphFilter.IsEmpty() && !Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }
        Graphs.Add(MakeShared<FJsonValueObject>(GraphToJson(Graph, Entry.Value, bIncludeGraphs, bIncludeJson, bIncludeT3D)));
    }

    if (!GraphFilter.IsEmpty() && Graphs.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph '%s' not found. Available graphs: %s"),
            *GraphFilter, *FString::Join(GraphNames, TEXT(", "))));
    }

    Result->SetArrayField(TEXT("graphs"), Graphs);
    return Result;
}
