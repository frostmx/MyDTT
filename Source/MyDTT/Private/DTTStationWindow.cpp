#include "DTTStationWindow.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
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
    const FName GStayButtonName(TEXT("DTTStayButton"));
    const FName GStayLabelName(TEXT("DTTStayLabel"));

    /** Said once per session: a window we cannot read is a thing to fix, not to report every 0.3 s. */
    bool GAnchorMissReported = false;
    bool GAnchorReported = false;

    /**
     * Where the button goes. The window is the game's, so nothing here may be load-bearing: the
     * hint under the vehicle settings is the first choice because it sits exactly where the button
     * belongs, the LOAD/UNLOAD switch is the second, and a window that offers neither keeps the
     * shape it shipped with.
     */
    UWidget* FindAnchor(UWidgetTree* Tree)
    {
        UWidget* ByHint = nullptr;
        UWidget* BySwitch = nullptr;
        Tree->ForEachWidget([&ByHint, &BySwitch](UWidget* Widget)
        {
            if (!IsValid(Widget) || Widget->IsA<UDTTStationStayButton>()) return;
            const FString Name = Widget->GetName();
            const FString Class = GetNameSafe(Widget->GetClass());
            if (Name.Contains(TEXT("VehichleSettings")) || Name.Contains(TEXT("VehicleSettings")))
            {
                if (!ByHint && Widget->GetParent()) ByHint = Widget;
            }
            else if (Class.Contains(TEXT("OnOffSwitch")))
            {
                // The last one wins: the vehicle settings are the bottom of that column, and a
                // fluid station carries another switch (the fuel one) above them.
                if (Widget->GetParent()) BySwitch = Widget;
            }
        });
        return ByHint ? ByHint : BySwitch;
    }

    void ApplyPadding(UWidget* Widget, const FMargin& Margin)
    {
        if (!IsValid(Widget)) return;
        if (UVerticalBoxSlot* V = Cast<UVerticalBoxSlot>(Widget->Slot)) { V->SetPadding(Margin); return; }
        if (UHorizontalBoxSlot* H = Cast<UHorizontalBoxSlot>(Widget->Slot)) { H->SetPadding(Margin); return; }
        if (UOverlaySlot* O = Cast<UOverlaySlot>(Widget->Slot)) O->SetPadding(Margin);
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

    void EnsureButton(UFGInteractWidget* Widget, AFGBuildableDockingStation* Station)
    {
        UWidgetTree* Tree = Widget->WidgetTree;
        if (!IsValid(Tree)) return;

        // The game caches its interact widgets, so the button usually survives the last station.
        if (UDTTStationStayButton* Existing = Cast<UDTTStationStayButton>(Tree->FindWidget(GStayButtonName)))
        {
            Existing->Attach(Station, Cast<UTextBlock>(Tree->FindWidget(GStayLabelName)));
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
                    TEXT("STATION_WINDOW_NO_ANCHOR Widget='%s': nothing in this window is a place this mod recognises, its button is left out."),
                    *GetNameSafe(Widget->GetClass()));
            }
            return;
        }

        UDTTStationStayButton* Button = Tree->ConstructWidget<UDTTStationStayButton>(UDTTStationStayButton::StaticClass(), GStayButtonName);
        UTextBlock* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), GStayLabelName);
        FSlateFontInfo Font = Label->GetFont();
        Font.Size = 13;
        Label->SetFont(Font);
        Label->SetJustification(ETextJustify::Center);
        Button->SetContent(Label);

        Parent->InsertChildAt(Parent->GetChildIndex(Anchor) + 1, Button);
        ApplyPadding(Button, FMargin(4.0f, 6.0f, 4.0f, 4.0f));
        Button->Attach(Station, Label);

        if (!GAnchorReported)
        {
            GAnchorReported = true;
            UE_LOG(LogDTTStationWindow, Display, TEXT("STATION_WINDOW_BUTTON Anchor='%s' AnchorClass='%s' Parent='%s'"),
                *Anchor->GetName(), *GetNameSafe(Anchor->GetClass()), *GetNameSafe(Parent->GetClass()));
        }
    }
}

void UDTTStationStayButton::Attach(AFGBuildableDockingStation* InStation, UTextBlock* InLabel)
{
    Label = InLabel;
    Station = InStation;
    // Bound once and only once: a cached window comes back with the delegate still on it.
    if (!OnClicked.IsAlreadyBound(this, &UDTTStationStayButton::HandleClicked))
        OnClicked.AddDynamic(this, &UDTTStationStayButton::HandleClicked);
    Refresh();
}

void UDTTStationStayButton::Refresh()
{
    if (!IsValid(Label)) return;
    const AFGBuildableDockingStation* Dock = Station.Get();
    const AFGDockingStationIdentifier* Identifier = IsValid(Dock) ? Dock->GetStationIdentifier() : nullptr;

    FString Base, Resource;
    EDTTDispatchRole Role = EDTTDispatchRole::None;
    int32 Threshold = 0;
    bool bStay = false;
    bHasDispatchTag = IsValid(Identifier) &&
        UDynamicTimeTableSubsystem::ParseDispatchTag(Identifier->GetStationName().ToString(), Base, Resource, Role, Threshold, &bStay) &&
        Role != EDTTDispatchRole::Producer;
    bStayUntilEmpty = bHasDispatchTag && bStay;

    SetIsEnabled(bHasDispatchTag);
    Label->SetText(FText::FromString(
        !bHasDispatchTag ? TEXT("Full unload: needs a (Resource) tag")
            : bStayUntilEmpty ? TEXT("Full unload: on") : TEXT("Full unload: off")));
    SetToolTipText(FText::FromString(TEXT(
        "On: a truck unloading here stands until its cargo is gone, topping the station up as it "
        "consumes. It is written as the = in the station's own tag, so the name always says what "
        "it does. A station that stops consuming keeps the truck.")));
}

void UDTTStationStayButton::HandleClicked()
{
    AFGBuildableDockingStation* Dock = Station.Get();
    UWorld* World = GetWorld();
    if (!IsValid(Dock) || !IsValid(World) || !bHasDispatchTag) return;
    const bool bWanted = !bStayUntilEmpty;

    if (World->GetNetMode() == NM_Client)
    {
        AFGPlayerController* PC = Cast<AFGPlayerController>(GetOwningPlayer());
        UDynamicTimeTableRemoteCallObject* RCO = IsValid(PC)
            ? Cast<UDynamicTimeTableRemoteCallObject>(PC->GetRemoteCallObjectOfClass(UDynamicTimeTableRemoteCallObject::StaticClass()))
            : nullptr;
        // The name comes back replicated and the next refresh reads it: what the button shows is
        // always what the station is called, never what the click hoped for.
        if (RCO) RCO->RequestSetStationStay(Dock, bWanted);
        return;
    }

    FString Message;
    if (UDynamicTimeTableSubsystem* Subsystem = World->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        Subsystem->SetStationStayUntilEmpty(Dock, bWanted, Message);
        Refresh();
    }
}

void FDTTStationWindow::Tick(UWorld* World)
{
    if (!IsValid(World) || World->GetNetMode() == NM_DedicatedServer) return;
    AFGPlayerController* PC = Cast<AFGPlayerController>(World->GetFirstPlayerController());
    UFGGameUI* GameUI = IsValid(PC) ? PC->GetGameUI() : nullptr;
    if (!IsValid(GameUI) || !GameUI->HasActiveInteractWidget()) return;

    for (UFGInteractWidget* Widget : GameUI->GetInteractWidgetStack())
    {
        if (AFGBuildableDockingStation* Station = StationOf(Widget)) EnsureButton(Widget, Station);
    }
}
