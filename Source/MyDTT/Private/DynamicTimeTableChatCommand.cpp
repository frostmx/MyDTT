#include "DynamicTimeTableChatCommand.h"

#include "Command/CommandSender.h"
#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableWidget.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "UI/FGGameUI.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "FGPlayerController.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableChatCommand, Log, All);

ADynamicTimeTableChatCommand::ADynamicTimeTableChatCommand()
{
    CommandName = TEXT("dtt");
    Usage = NSLOCTEXT(
        "DynamicTimeTable",
        "ChatCommand.DTT.Usage",
        "/dtt - Open DynamicTimeTable; /dtt groups - list groups; /dtt dispatch [resource] - list resource dispatch, optionally filtered to one resource; /dtt recalc - rebuild active bookings");
    Aliases.Add(TEXT("dynamictimetable"));
}

EExecutionStatus ADynamicTimeTableChatCommand::ExecuteCommand_Implementation(
    UCommandSender* Sender,
    const TArray<FString>& Arguments,
    const FString& Label)
{
    if (!IsValid(Sender) || !Sender->IsPlayerSender())
    {
        if (IsValid(Sender))
        {
            Sender->SendChatMessage(
                TEXT("/dtt can only be used by a player."),
                FLinearColor::Red);
        }
        return EExecutionStatus::UNCOMPLETED;
    }

    AFGPlayerController* Player = Sender->GetPlayer();
    UWorld* World = IsValid(Player) ? Player->GetWorld() : nullptr;
    UDynamicTimeTableSubsystem* Subsystem = IsValid(World)
        ? World->GetSubsystem<UDynamicTimeTableSubsystem>()
        : nullptr;

    if (!IsValid(Subsystem))
    {
        Sender->SendChatMessage(
            TEXT("DynamicTimeTable is not available in this world."),
            FLinearColor::Red);
        return EExecutionStatus::UNCOMPLETED;
    }

    if (Arguments.Num() >= 1 &&
        Arguments[0].Equals(TEXT("groups"), ESearchCase::IgnoreCase))
    {
        const TArray<FDTTStationGroup>& Groups = Subsystem->GetStationGroups();
        if (Groups.Num() == 0)
        {
            Sender->SendChatMessage(
                TEXT("dTT: no groups. Add \"[name]\" to a station name to put it in a group, "
                     "or \"[name,3]\" to give it three slots."),
                FLinearColor(0.20f, 0.80f, 1.00f));
            return EExecutionStatus::COMPLETED;
        }

        for (const FDTTStationGroup& Group : Groups)
        {
            TArray<FDTTStationRuntimeLoad> Loads;
            Subsystem->GetGroupRuntimeLoad(Group.GroupId, Loads);
            FString Line = FString::Printf(TEXT("%s [%s]:"), *Group.GroupName,
                Group.Kind == EDTTGroupKind::Truck ? TEXT("Trucks") : TEXT("Trains"));
            for (const FDTTStationRuntimeLoad& Load : Loads)
            {
                Line += FString::Printf(TEXT("  %s %d/%d%s"),
                    *UDynamicTimeTableSubsystem::StripStationTag(Load.StationName),
                    Load.AssignedTrains, Load.TargetSlots,
                    Load.bOverloaded ? TEXT(" OVERLOAD") : TEXT(""));
            }
            if (Loads.Num() == 0) Line += TEXT("  (empty)");
            Sender->SendChatMessage(Line, FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 &&
        Arguments[0].Equals(TEXT("dispatch"), ESearchCase::IgnoreCase))
    {
        const FString ResourceFilter = Arguments.Num() >= 2 ? Arguments[1] : FString();
        TArray<FString> Lines;
        Subsystem->GetDispatchStatusLines(Lines, ResourceFilter);
        for (const FString& Line : Lines)
        {
            Sender->SendChatMessage(Line, FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 &&
        Arguments[0].Equals(TEXT("recalc"), ESearchCase::IgnoreCase))
    {
        Subsystem->RecalculateBookingsFromVanilla();
        Sender->SendChatMessage(
            TEXT("dTT: Active bookings were rebuilt from Vanilla timetables and truck routes."),
            FLinearColor(0.20f, 0.80f, 1.00f));
        return EExecutionStatus::COMPLETED;
    }

    if (!Player->IsLocalController())
    {
        UDynamicTimeTableRemoteCallObject* RemoteCallObject = Cast<UDynamicTimeTableRemoteCallObject>(
            Player->GetRemoteCallObjectOfClass(UDynamicTimeTableRemoteCallObject::StaticClass()));
        if (!IsValid(RemoteCallObject))
        {
            // The channel is only created for a player who has the mod. Creating one here for a
            // player who does not would send them a subobject of a class they cannot resolve and
            // drop their connection - so say so instead.
            Sender->SendChatMessage(
                TEXT("dTT: the window needs the mod installed on your client. "
                     "The server balances without it; /dtt groups shows the state."),
                FLinearColor::Red);
            return EExecutionStatus::UNCOMPLETED;
        }
        RemoteCallObject->Client_OpenWindow();
        return EExecutionStatus::COMPLETED;
    }

    UFGGameUI* GameUI = Player->GetGameUI();
    if (!IsValid(GameUI))
    {
        Sender->SendChatMessage(TEXT("dTT: Game UI is not ready."), FLinearColor::Red);
        return EExecutionStatus::UNCOMPLETED;
    }

    TArray<UUserWidget*> ExistingWidgets;
    UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
        Player,
        ExistingWidgets,
        UDynamicTimeTableWidget::StaticClass(),
        true);

    for (UUserWidget* ExistingWidget : ExistingWidgets)
    {
        if (IsValid(ExistingWidget))
        {
            ExistingWidget->RemoveFromParent();
            UE_LOG(LogDynamicTimeTableChatCommand, Verbose, TEXT("dTT: existing virtual window closed by /dtt toggle."));
            return EExecutionStatus::COMPLETED;
        }
    }

    UDynamicTimeTableWidget* Window = CreateWidget<UDynamicTimeTableWidget>(
        Player,
        UDynamicTimeTableWidget::StaticClass());
    if (!IsValid(Window))
    {
        Sender->SendChatMessage(TEXT("dTT: Could not create the virtual window."), FLinearColor::Red);
        UE_LOG(LogDynamicTimeTableChatCommand, Error, TEXT("dTT: CreateWidget returned null."));
        return EExecutionStatus::UNCOMPLETED;
    }

    // AddToViewport is used deliberately here. The Satisfactory AddInteractWidget
    // Blueprint event only updates the stack for native widgets in this build and
    // did not attach the pure C++ widget to the viewport.
    Window->AddToViewport(1000);

    FInputModeGameAndUI InputMode;
    InputMode.SetWidgetToFocus(Window->TakeWidget());
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    InputMode.SetHideCursorDuringCapture(false);
    Player->SetInputMode(InputMode);
    Player->SetShowMouseCursor(true);
    Player->bEnableClickEvents = true;
    Player->bEnableMouseOverEvents = true;
    Player->CurrentMouseCursor = EMouseCursor::Default;
    Player->SetIgnoreMoveInput(true);
    Player->SetIgnoreLookInput(true);

    UE_LOG(LogDynamicTimeTableChatCommand, Verbose, TEXT("dTT: virtual window attached directly to viewport."));
    return EExecutionStatus::COMPLETED;
}
