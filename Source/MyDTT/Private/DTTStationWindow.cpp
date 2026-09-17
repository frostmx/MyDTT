#include "DTTStationWindow.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "UI/FGGameUI.h"
#include "UI/FGInteractWidget.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"

DEFINE_LOG_CATEGORY_STATIC(LogDTTStationWindow, Log, All);

namespace
{
    const FName GControlName(TEXT("DTTStayControl"));
    const FName GSwitchName(TEXT("DTTStaySwitch"));
    const FName GSwitchChangedProperty(TEXT("onSwitchChanged"));

    /** The gap above our row, and what to assume its height is before the window has been drawn. */
    constexpr float GRowGap = 8.0f;
    constexpr float GRowHeightFallback = 30.0f;

    /** Said once per session: a window we cannot read is a thing to fix, not to report every 0.3 s. */
    bool GAnchorMissReported = false;
    bool GAnchorReported = false;

    /**
     * The window the control was last found in. The watcher comes back three times a second while a
     * station window is open, and without this every pass walked the whole widget tree looking for
     * a control that is already known.
     */
    TWeakObjectPtr<UFGInteractWidget> GLastWindow;
    TWeakObjectPtr<UDTTStationStayControl> GLastControl;

    /** The station window that was open on the last pass; null when there was none. */
    TWeakObjectPtr<UFGInteractWidget> GOpenWindow;

    /**
     * Where the control goes: right under the UNLOAD/LOAD switch, in the box that holds the
     * vehicle settings. That switch is a BPW_OnOffSwitch, and so is the fuel one above it in a
     * fluid station - they are told apart by name, the fuel one being mForceFluidSwitch.
     *
     * Only a box will do as a parent. The window is laid out on canvas panels, where a child needs
     * coordinates of its own and a new one lands in the corner with no size at all - which is
     * exactly where the first version of this put its button. A window that offers no box keeps
     * the shape it shipped with.
     */
    UWidget* FindAnchor(UWidgetTree* Tree)
    {
        UWidget* ByName = nullptr;
        UWidget* ByClass = nullptr;
        Tree->ForEachWidget([&ByName, &ByClass](UWidget* Widget)
        {
            if (!IsValid(Widget) || Widget->IsA<UDTTStationStayControl>()) return;
            if (!GetNameSafe(Widget->GetClass()).Contains(TEXT("OnOffSwitch"))) return;
            const FString Name = Widget->GetName();
            if (Name.Contains(TEXT("Fluid")) || Name.Contains(TEXT("Fuel"))) return;

            UPanelWidget* Parent = Widget->GetParent();
            if (!Parent || !(Parent->IsA<UVerticalBox>() || Parent->IsA<UHorizontalBox>())) return;
            if (Name.Equals(TEXT("BPW_OnOffSwitch"))) { if (!ByName) ByName = Widget; }
            else if (!ByClass) ByClass = Widget;
        });
        return ByName ? ByName : ByClass;
    }

    /**
     * The window's name field, which is where the trouble was: it keeps the text the window opened
     * with and writes that text back to the station whenever it loses focus - and a click on the
     * switch below is enough to do that. So a name changed underneath it came back a second later
     * as the old one, and the marker this switch had just set was gone again.
     */
    UWidget* FindNameField(UWidgetTree* Tree)
    {
        if (UWidget* ByName = Tree->FindWidget(TEXT("mStationNameTextObject"))) return ByName;

        UWidget* Found = nullptr;
        Tree->ForEachWidget([&Found](UWidget* Widget)
        {
            if (Found || !IsValid(Widget)) return;
            if (Widget->IsA<UEditableText>() || Widget->IsA<UEditableTextBox>()) Found = Widget;
        });
        return Found;
    }

    /** Puts the station's name into that field, unless the player is typing their own in it. */
    void SyncNameField(UWidget* Field, const FString& Name)
    {
        if (!IsValid(Field) || Name.IsEmpty() || Field->HasKeyboardFocus()) return;
        if (UEditableText* Text = Cast<UEditableText>(Field))
        {
            if (!Text->GetText().ToString().Equals(Name)) Text->SetText(FText::FromString(Name));
            return;
        }
        if (UEditableTextBox* Box = Cast<UEditableTextBox>(Field))
        {
            if (!Box->GetText().ToString().Equals(Name)) Box->SetText(FText::FromString(Name));
        }
    }

    void ApplyPadding(UWidget* Widget, const FMargin& Margin)
    {
        if (!IsValid(Widget)) return;
        if (UVerticalBoxSlot* V = Cast<UVerticalBoxSlot>(Widget->Slot))
        {
            V->SetPadding(Margin);
            V->SetHorizontalAlignment(HAlign_Fill);
            return;
        }
        if (UHorizontalBoxSlot* H = Cast<UHorizontalBoxSlot>(Widget->Slot))
        {
            H->SetPadding(Margin);
            H->SetVerticalAlignment(VAlign_Center);
        }
    }

    /**
     * The station a docking-station window is showing, or null for any other interact widget - a
     * train platform's window is a different Blueprint and its interact object is not this class,
     * so this one test keeps us out of every window that is not a truck station's.
     *
     * The window also keeps the station in a variable of its own; that is read only if the interact
     * object turns out to be something else, since a Blueprint variable is the game's to rename.
     */
    AFGBuildableDockingStation* StationOf(UFGInteractWidget* Widget)
    {
        if (!IsValid(Widget)) return nullptr;
        if (AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Widget->mInteractObject)) return Station;

        const FObjectProperty* Property = FindFProperty<FObjectProperty>(Widget->GetClass(), TEXT("mDockingStation"));
        return Property ? Cast<AFGBuildableDockingStation>(Property->GetObjectPropertyValue_InContainer(Widget)) : nullptr;
    }

    void EnsureControl(UFGInteractWidget* Widget, AFGBuildableDockingStation* Station, bool bWindowOpened)
    {
        UWidgetTree* Tree = Widget->WidgetTree;
        if (!IsValid(Tree)) return;

        if (GLastWindow.Get() == Widget && GLastControl.IsValid())
        {
            GLastControl->Attach(Station, bWindowOpened);
            return;
        }

        // The game caches its interact widgets, so the control usually survives the last station.
        if (UDTTStationStayControl* Existing = Cast<UDTTStationStayControl>(Tree->FindWidget(GControlName)))
        {
            GLastWindow = Widget;
            GLastControl = Existing;
            Existing->Attach(Station, bWindowOpened);
            return;
        }

        UWidget* Anchor = FindAnchor(Tree);
        UPanelWidget* Parent = IsValid(Anchor) ? Anchor->GetParent() : nullptr;
        if (!IsValid(Parent))
        {
            if (!GAnchorMissReported)
            {
                GAnchorMissReported = true;
                UE_LOG(LogDTTStationWindow, Warning,
                    TEXT("STATION_WINDOW_NO_ANCHOR Widget='%s': nothing in this window is a place this mod recognises, its switch is left out."),
                    *GetNameSafe(Widget->GetClass()));
            }
            return;
        }

        UDTTStationStayControl* Control = Tree->ConstructWidget<UDTTStationStayControl>(UDTTStationStayControl::StaticClass(), GControlName);
        Control->Build(Tree, Anchor->GetClass());
        Parent->InsertChildAt(Parent->GetChildIndex(Anchor) + 1, Control);
        // The same gap the window leaves around its own divider, so the two switches read as one
        // block of settings instead of as a row that wandered off down the panel.
        ApplyPadding(Control, FMargin(0.0f, GRowGap, 0.0f, 0.0f));
        GLastWindow = Widget;
        GLastControl = Control;
        const bool bFirstTime = true;
        // That block sits on a canvas and grows downward from a fixed point, so the row just added
        // would hang over the bottom edge of the panel. The block is moved up by half of what the
        // row takes: the two switches then share the space the one of them used to have, which
        // keeps the block clear of the edge without pulling it up off its own place. Done once,
        // when the row is added, and only for a block placed this way.
        if (UCanvasPanelSlot* BoxSlot = Cast<UCanvasPanelSlot>(Parent->Slot))
        {
            const float Height = Anchor->GetCachedGeometry().GetLocalSize().Y;
            FMargin Offsets = BoxSlot->GetOffsets();
            Offsets.Top -= ((Height > 1.0f ? Height : GRowHeightFallback) + GRowGap) * 0.5f;
            BoxSlot->SetOffsets(Offsets);
        }
        Control->Attach(Station, bFirstTime);

        if (!GAnchorReported)
        {
            GAnchorReported = true;
            UE_LOG(LogDTTStationWindow, Display, TEXT("STATION_WINDOW_CONTROL Anchor='%s' AnchorClass='%s' Parent='%s'"),
                *Anchor->GetName(), *GetNameSafe(Anchor->GetClass()), *GetNameSafe(Parent->GetClass()));
        }
    }

    /**
     * What the switch is showing right now. SetIsOn is a Blueprint event that drives the check box
     * underneath, which in turn plays the animation and the sound and broadcasts the change - so it
     * is only ever called when this disagrees with the station's name. Called every pass, as the
     * first version did, it re-triggered all of that three times a second.
     */
    bool ReadSwitchIsOn(const UUserWidget* Target, bool& bOutIsOn)
    {
        const FBoolProperty* Property = IsValid(Target)
            ? FindFProperty<FBoolProperty>(Target->GetClass(), TEXT("mIsOn")) : nullptr;
        if (!Property) return false;
        bOutIsOn = Property->GetPropertyValue_InContainer(Target);
        return true;
    }

    /** Calls a Blueprint function of one bool, the way SetIsOn is declared. */
    void CallBoolFunction(UUserWidget* Target, const TCHAR* FunctionName, bool bValue)
    {
        UFunction* Function = IsValid(Target) ? Target->FindFunction(FName(FunctionName)) : nullptr;
        if (!Function || Function->ParmsSize == 0) return;

        TArray<uint8> Parameters;
        Parameters.SetNumZeroed(Function->ParmsSize);
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            if (FBoolProperty* Bool = CastField<FBoolProperty>(*It))
            {
                Bool->SetPropertyValue_InContainer(Parameters.GetData(), bValue);
                break;
            }
        }
        Target->ProcessEvent(Function, Parameters.GetData());
    }

    /** SetOnOffTexts(FText mOnText, FText mOffText), filled in the order the parameters come. */
    void CallSetOnOffTexts(UUserWidget* Target, const FText& OnText, const FText& OffText)
    {
        UFunction* Function = IsValid(Target) ? Target->FindFunction(TEXT("SetOnOffTexts")) : nullptr;
        if (!Function || Function->ParmsSize == 0) return;

        TArray<uint8> Parameters;
        Parameters.SetNumZeroed(Function->ParmsSize);
        int32 Index = 0;
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            if (FTextProperty* Text = CastField<FTextProperty>(*It))
            {
                Text->SetPropertyValue_InContainer(Parameters.GetData(), Index == 0 ? OnText : OffText);
                ++Index;
            }
        }
        Target->ProcessEvent(Function, Parameters.GetData());
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
            It->DestroyValue_InContainer(Parameters.GetData());
    }
}

void UDTTStationStayControl::Build(UWidgetTree* Tree, UClass* SwitchClass)
{
    if (!IsValid(Tree)) return;

    SetToolTipText(FText::FromString(TEXT(
        "FULL: the truck stands here until its cargo is gone. It is the = in the station's tag.")));
    NameField = FindNameField(Tree);

    // The game's own switch, by the class of the one above it: no asset path to go stale, and the
    // animation, the sound and the styling come with it.
    if (SwitchClass && SwitchClass->IsChildOf(UUserWidget::StaticClass()))
    {
        Switch = Tree->ConstructWidget<UUserWidget>(SwitchClass, GSwitchName);
        if (FMulticastDelegateProperty* Changed = FindFProperty<FMulticastDelegateProperty>(SwitchClass, GSwitchChangedProperty))
        {
            FScriptDelegate Delegate;
            Delegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UDTTStationStayControl, HandleSwitchChanged));
            Changed->AddDelegate(MoveTemp(Delegate), Switch);
        }
        else
        {
            // A switch nobody can listen to would flip and do nothing at all.
            Switch = nullptr;
        }
    }

    if (Switch)
    {
        CallSetOnOffTexts(Switch, FText::FromString(TEXT("FULL")), FText::FromString(TEXT("NORMAL")));
        if (UVerticalBoxSlot* SwitchSlot = Cast<UVerticalBoxSlot>(AddChildToVerticalBox(Switch)))
            SwitchSlot->SetHorizontalAlignment(HAlign_Center);
        return;
    }

    // No switch to be had: a plain button says the same thing and still works.
    Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), GSwitchName);
    ButtonLabel = Tree->ConstructWidget<UTextBlock>();
    ButtonLabel->SetJustification(ETextJustify::Center);
    Button->SetContent(ButtonLabel);
    Button->OnClicked.AddDynamic(this, &UDTTStationStayControl::HandleButtonClicked);
    if (UVerticalBoxSlot* ButtonSlot = Cast<UVerticalBoxSlot>(AddChildToVerticalBox(Button)))
        ButtonSlot->SetHorizontalAlignment(HAlign_Center);
}

void UDTTStationStayControl::Attach(AFGBuildableDockingStation* InStation, bool bWindowOpened)
{
    Station = InStation;
    if (bWindowOpened) bSwitchWritten = false;
    Refresh();
}

void UDTTStationStayControl::Refresh()
{
    const AFGBuildableDockingStation* Dock = Station.Get();
    const AFGDockingStationIdentifier* Identifier = IsValid(Dock) ? Dock->GetStationIdentifier() : nullptr;
    const FString Name = IsValid(Identifier) ? Identifier->GetStationName().ToString() : FString();

    // The name is the state, and this runs three times a second: taking the same string apart again
    // tells us what we already know. Which station it came from does not matter - the same name
    // parses to the same thing. What is never skipped is the switch below: that comparison is what
    // takes a flip back when the server refuses it.
    if (Name != LastName)
    {
        LastName = Name;

        FString Base, Resource;
        EDTTDispatchRole Role = EDTTDispatchRole::None;
        int32 Threshold = 0;
        bool bStay = false;
        bHasDispatchTag = !Name.IsEmpty() &&
            UDynamicTimeTableSubsystem::ParseDispatchTag(Name, Base, Resource, Role, Threshold, &bStay) &&
            Role != EDTTDispatchRole::Producer;
        bStayUntilEmpty = bHasDispatchTag && bStay;

        // A station with no dispatch tag has nothing to say here, and the window keeps the shape
        // the game gave it.
        const ESlateVisibility Wanted = bHasDispatchTag
            ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed;
        if (GetVisibility() != Wanted) SetVisibility(Wanted);

        if (IsValid(ButtonLabel))
            ButtonLabel->SetText(FText::FromString(bStayUntilEmpty ? TEXT("Full unload: on") : TEXT("Full unload: off")));

        // The name moved - by this switch, by auto-naming, by another player - so the field that
        // will write it back has to hear about it.
        SyncNameField(NameField.Get(), Name);
    }

    // Written on every pass, not only on a click: the name is the state, so a flip the server
    // refused is taken back here within a third of a second. Only when it really differs, though -
    // see ReadSwitchIsOn.
    //
    // Once per opened window it is written whatever it says, because a switch that has never been
    // told anything is drawn in the state its Blueprint was saved in while its variable says the
    // other one - the game writes its own switches on opening a window for the same reason. Left
    // out, that showed as a switch painted on with its knob off, and as a first click that only
    // brought the two together instead of doing anything.
    bool bShown = bSwitchShows;
    ReadSwitchIsOn(Switch, bShown);
    if (Switch && (!bSwitchWritten || bShown != bStayUntilEmpty))
    {
        CallBoolFunction(Switch, TEXT("SetIsOn"), bStayUntilEmpty);
        bSwitchShows = bStayUntilEmpty;
        bSwitchWritten = true;
    }
}

void UDTTStationStayControl::HandleSwitchChanged(bool bIsOn)
{
    Request(bIsOn);
}

void UDTTStationStayControl::HandleButtonClicked()
{
    Request(!bStayUntilEmpty);
}

void UDTTStationStayControl::Request(bool bOn)
{
    AFGBuildableDockingStation* Dock = Station.Get();
    UWorld* World = GetWorld();
    if (!IsValid(Dock) || !IsValid(World) || !bHasDispatchTag || bOn == bStayUntilEmpty) return;

    if (World->GetNetMode() == NM_Client)
    {
        AFGPlayerController* PC = Cast<AFGPlayerController>(GetOwningPlayer());
        UDynamicTimeTableRemoteCallObject* RCO = IsValid(PC)
            ? Cast<UDynamicTimeTableRemoteCallObject>(PC->GetRemoteCallObjectOfClass(UDynamicTimeTableRemoteCallObject::StaticClass()))
            : nullptr;
        // The name comes back replicated and the next refresh reads it: what the control shows is
        // always what the station is called, never what the click hoped for.
        if (RCO) RCO->RequestSetStationStay(Dock, bOn);
        return;
    }

    FString Message;
    if (UDynamicTimeTableSubsystem* Subsystem = World->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        Subsystem->SetStationStayUntilEmpty(Dock, bOn, Message);
        Refresh();
    }
}

void FDTTStationWindow::Tick(UWorld* World)
{
    if (!IsValid(World) || World->GetNetMode() == NM_DedicatedServer) return;
    AFGPlayerController* PC = Cast<AFGPlayerController>(World->GetFirstPlayerController());
    UFGGameUI* GameUI = IsValid(PC) ? PC->GetGameUI() : nullptr;
    if (!IsValid(GameUI) || !GameUI->HasActiveInteractWidget()) return;

    // Which window was up last pass, so one that has just been opened - a new one, or the same one
    // opened again, since the game keeps its interact widgets - can be told from one left standing.
    UFGInteractWidget* Open = nullptr;
    for (UFGInteractWidget* Widget : GameUI->GetInteractWidgetStack())
    {
        if (AFGBuildableDockingStation* Station = StationOf(Widget))
        {
            EnsureControl(Widget, Station, GOpenWindow.Get() != Widget);
            Open = Widget;
        }
    }
    GOpenWindow = Open;
}
