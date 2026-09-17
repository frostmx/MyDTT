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
        "/dtt - Open DynamicTimeTable; /dtt groups - list groups; /dtt dispatch [resource] - list resource dispatch, optionally filtered to one resource; /dtt trucks [resource] - list dispatch and pool trucks; /dtt why <truck> - what one truck is doing and what stands in its way; /dtt depots - list depots and pool trucks; /dtt park <truck> - send a truck to the nearest free depot; /dtt release <truck> - let a parked truck go; /dtt settings - show the settings; /dtt set <field> <value> - change one setting; /dtt autoname [apply|strip|gamenames [apply]|<station>] - name stations from their belts; /dtt paint [on|off] - colour autopilot trucks by what they carry; /dtt recalc - rebuild active bookings");
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
        Arguments[0].Equals(TEXT("trucks"), ESearchCase::IgnoreCase))
    {
        const FString ResourceFilter = Arguments.Num() >= 2 ? Arguments[1] : FString();
        TArray<FString> Lines;
        Subsystem->GetTruckStatusLines(Lines, ResourceFilter);
        for (const FString& Line : Lines)
        {
            Sender->SendChatMessage(Line, FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 && Arguments[0].Equals(TEXT("why"), ESearchCase::IgnoreCase))
    {
        // The truck name may contain spaces ("Iron Ore -> Smelter A"); everything after the verb is it.
        FString Query;
        for (int32 Index = 1; Index < Arguments.Num(); ++Index)
        {
            if (!Query.IsEmpty()) Query += TEXT(" ");
            Query += Arguments[Index];
        }
        if (Query.IsEmpty())
        {
            Sender->SendChatMessage(
                TEXT("dTT: /dtt why <truck> - the Name#number from /dtt trucks, the number alone, or the display name."),
                FLinearColor::Red);
            return EExecutionStatus::BAD_ARGUMENTS;
        }
        TArray<FString> Lines;
        Subsystem->GetTruckWhyLines(Query, Lines);
        for (const FString& Line : Lines)
        {
            Sender->SendChatMessage(Line, FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 &&
        Arguments[0].Equals(TEXT("depots"), ESearchCase::IgnoreCase))
    {
        TArray<FString> Lines;
        Subsystem->GetDepotStatusLines(Lines);
        for (const FString& Line : Lines)
        {
            Sender->SendChatMessage(Line, FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 && Arguments[0].Equals(TEXT("settings"), ESearchCase::IgnoreCase))
    {
        Sender->SendChatMessage(FString::Printf(TEXT("dTT settings: %s"), *Subsystem->GetSettings().Describe()), FLinearColor(0.20f, 0.80f, 1.00f));
        Sender->SendChatMessage(TEXT("dTT: /dtt set <field> <value>, fields as listed (case-insensitive); the Settings tab of /dtt does the same."), FLinearColor(0.20f, 0.80f, 1.00f));
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 && Arguments[0].Equals(TEXT("set"), ESearchCase::IgnoreCase))
    {
        if (Arguments.Num() < 3)
        {
            Sender->SendChatMessage(TEXT("dTT: /dtt set <field> <value> - see /dtt settings for the fields."), FLinearColor::Red);
            return EExecutionStatus::BAD_ARGUMENTS;
        }
        if (Arguments[1].Equals(TEXT("naminglanguage"), ESearchCase::IgnoreCase) &&
            (Arguments[2] == TEXT("?") || Arguments[2].Equals(TEXT("list"), ESearchCase::IgnoreCase)))
        {
            TArray<FString> Parts;
            for (const FDTTLanguageOption& Language : FDTTItemNames::ListLanguages())
            {
                Parts.Add(FString::Printf(TEXT("%s = %s"), *Language.Code, *Language.Label));
            }
            // A chat line holds about ten of these.
            constexpr int32 PerLine = 10;
            for (int32 Index = 0; Index < Parts.Num(); Index += PerLine)
            {
                TArray<FString> Chunk;
                for (int32 J = Index; J < Parts.Num() && J < Index + PerLine; ++J) Chunk.Add(Parts[J]);
                Sender->SendChatMessage(FString::Printf(TEXT("dTT languages: %s"), *FString::Join(Chunk, TEXT(", "))), FLinearColor(0.20f, 0.80f, 1.00f));
            }
            return EExecutionStatus::COMPLETED;
        }
        FDTTSettings Wanted = Subsystem->GetSettings();
        if (!Wanted.SetByName(Arguments[1], Arguments[2]))
        {
            Sender->SendChatMessage(FString::Printf(TEXT("dTT: unknown field or value: %s %s"), *Arguments[1], *Arguments[2]), FLinearColor::Red);
            return EExecutionStatus::BAD_ARGUMENTS;
        }
        FString Message;
        const bool bOk = Subsystem->ApplySettings(Wanted, TEXT("chat"), &Message);
        Sender->SendChatMessage(Message, bOk ? FLinearColor(0.20f, 0.80f, 1.00f) : FLinearColor::Red);
        return bOk ? EExecutionStatus::COMPLETED : EExecutionStatus::UNCOMPLETED;
    }

    if (Arguments.Num() >= 1 &&
        (Arguments[0].Equals(TEXT("park"), ESearchCase::IgnoreCase) ||
         Arguments[0].Equals(TEXT("release"), ESearchCase::IgnoreCase)))
    {
        // The truck name may contain spaces ("Iron Ore → Smelter A"); everything after the verb is it.
        FString Query;
        for (int32 Index = 1; Index < Arguments.Num(); ++Index)
        {
            if (!Query.IsEmpty()) Query += TEXT(" ");
            Query += Arguments[Index];
        }
        if (Query.IsEmpty())
        {
            Sender->SendChatMessage(
                FString::Printf(TEXT("dTT: /dtt %s <truck> - the Name#number from /dtt depots, the number alone, or the display name."), *Arguments[0].ToLower()),
                FLinearColor::Red);
            return EExecutionStatus::BAD_ARGUMENTS;
        }
        FString Message;
        const bool bOk = Arguments[0].Equals(TEXT("park"), ESearchCase::IgnoreCase)
            ? Subsystem->CommandParkTruck(Query, Message)
            : Subsystem->CommandReleaseTruck(Query, Message);
        Sender->SendChatMessage(Message, bOk ? FLinearColor(0.20f, 0.80f, 1.00f) : FLinearColor::Red);
        return bOk ? EExecutionStatus::COMPLETED : EExecutionStatus::UNCOMPLETED;
    }

    if (Arguments.Num() >= 1 && Arguments[0].Equals(TEXT("autoname"), ESearchCase::IgnoreCase))
    {
        const FString Verb = Arguments.Num() >= 2 ? Arguments[1] : FString();
        TArray<FString> Lines;

        if (Verb.Equals(TEXT("apply"), ESearchCase::IgnoreCase))
        {
            const int32 Changed = Subsystem->RunAutoNamePass(true, false, &Lines);
            Lines.Add(FString::Printf(TEXT("dTT: %d station(s) renamed."), Changed));
        }
        else if (Verb.Equals(TEXT("strip"), ESearchCase::IgnoreCase))
        {
            const int32 Stripped = Subsystem->StripAutoNames(&Lines);
            Lines.Add(FString::Printf(TEXT("dTT: %d station(s) stripped."), Stripped));
        }
        else if (Verb.Equals(TEXT("gamenames"), ESearchCase::IgnoreCase))
        {
            const bool bApply = Arguments.Num() >= 3 && Arguments[2].Equals(TEXT("apply"), ESearchCase::IgnoreCase);
            const int32 Changed = Subsystem->RestoreGameWords(bApply, Lines);
            Lines.Add(bApply
                ? FString::Printf(TEXT("dTT: %d station(s) renamed."), Changed)
                : FString::Printf(TEXT("dTT: %d station(s) would change. /dtt autoname gamenames apply writes them."), Changed));
        }
        else if (!Verb.IsEmpty())
        {
            // The station name may contain spaces; everything after the verb is the query.
            FString Query;
            for (int32 Index = 1; Index < Arguments.Num(); ++Index)
            {
                if (!Query.IsEmpty()) Query += TEXT(" ");
                Query += Arguments[Index];
            }
            Subsystem->DescribeAutoName(Query, Lines);
        }
        else
        {
            const int32 Changed = Subsystem->RunAutoNamePass(true, true, &Lines);
            Lines.Add(FString::Printf(TEXT("dTT: %d station(s) would change. /dtt autoname apply writes them."), Changed));
        }

        // Chat drops long bursts, and a big factory has a lot of docks.
        constexpr int32 MaxChatLines = 40;
        for (int32 Index = 0; Index < Lines.Num() && Index < MaxChatLines; ++Index)
        {
            Sender->SendChatMessage(Lines[Index], FLinearColor(0.20f, 0.80f, 1.00f));
        }
        if (Lines.Num() > MaxChatLines)
        {
            Sender->SendChatMessage(
                FString::Printf(TEXT("dTT: %d more line(s) in the log (LogDynamicTimeTableSubsystem)."), Lines.Num() - MaxChatLines),
                FLinearColor(0.20f, 0.80f, 1.00f));
        }
        return EExecutionStatus::COMPLETED;
    }

    if (Arguments.Num() >= 1 && Arguments[0].Equals(TEXT("paint"), ESearchCase::IgnoreCase))
    {
        if (Arguments.Num() < 2)
        {
            Sender->SendChatMessage(
                FString::Printf(TEXT("dTT: cargo paint is %s; %d truck(s) wear a cargo colour. /dtt paint on|off"),
                    Subsystem->IsPaintByCargoEnabled() ? TEXT("on") : TEXT("off"),
                    Subsystem->GetPaintedTruckCount()),
                FLinearColor(0.20f, 0.80f, 1.00f));
            return EExecutionStatus::COMPLETED;
        }
        const bool bOn = Arguments[1].Equals(TEXT("on"), ESearchCase::IgnoreCase);
        if (!bOn && !Arguments[1].Equals(TEXT("off"), ESearchCase::IgnoreCase))
        {
            Sender->SendChatMessage(TEXT("dTT: /dtt paint on|off"), FLinearColor::Red);
            return EExecutionStatus::BAD_ARGUMENTS;
        }
        const bool bWas = Subsystem->IsPaintByCargoEnabled();
        Subsystem->SetPaintByCargo(bOn, TEXT("chat"));
        Sender->SendChatMessage(
            bWas == bOn
                ? FString::Printf(TEXT("dTT: cargo paint was already %s."), bOn ? TEXT("on") : TEXT("off"))
                : (bOn
                    ? FString(TEXT("dTT: cargo paint on. Autopilot trucks take the colour of their cargo and their own paint comes back when they empty."))
                    : FString(TEXT("dTT: cargo paint off. Every truck this mod painted has its own colour back."))),
            FLinearColor(0.20f, 0.80f, 1.00f));
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
