#include "DTTHotkeys.h"
#include "DynamicTimeTableWidget.h"
#include "FGPlayerController.h"
#include "FGPlayerInput.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/ConfigCacheIni.h"

DEFINE_LOG_CATEGORY_STATIC(LogDTTHotkeys, Log, All);

namespace
{
    const TCHAR* DTTHotkeySection = TEXT("MyDTT.Hotkeys");
    /** The ini keys of the tabs, in EDTTTab order. */
    const TCHAR* DTTTabIniNames[] = { TEXT("Dispatch"), TEXT("Trucks"), TEXT("Stations"), TEXT("Drones"), TEXT("Timetable"), TEXT("Groups"), TEXT("Settings") };
    static_assert(UE_ARRAY_COUNT(DTTTabIniNames) == int32(EDTTTab::Count), "one ini key per tab");

    FKey DTTKeyFromIni(const FString& Value)
    {
        const FKey Key(*Value.TrimStartAndEnd());
        return UDTTHotkeys::Choices().Contains(Key) ? Key : EKeys::Invalid;
    }
}

const TArray<FKey>& UDTTHotkeys::Choices()
{
    static const TArray<FKey> Keys = {
        EKeys::Invalid, EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6,
        EKeys::F7, EKeys::F8, EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12 };
    return Keys;
}

FString UDTTHotkeys::KeyLabel(const FKey& Key)
{
    return Key.IsValid() ? Key.GetDisplayName().ToString() : FString(TEXT("None"));
}

void UDTTHotkeys::Load()
{
    TabKeys.Init(EKeys::Invalid, int32(EDTTTab::Count));
    WindowKey = EKeys::F1;
    if (!GConfig) return;
    FString Value;
    if (GConfig->GetString(DTTHotkeySection, TEXT("Window"), Value, GGameUserSettingsIni)) WindowKey = DTTKeyFromIni(Value);
    for (int32 Tab = 0; Tab < TabKeys.Num(); ++Tab)
    {
        if (GConfig->GetString(DTTHotkeySection, DTTTabIniNames[Tab], Value, GGameUserSettingsIni)) TabKeys[Tab] = DTTKeyFromIni(Value);
    }
}

void UDTTHotkeys::Save() const
{
    if (!GConfig) return;
    GConfig->SetString(DTTHotkeySection, TEXT("Window"), *WindowKey.ToString(), GGameUserSettingsIni);
    for (int32 Tab = 0; Tab < TabKeys.Num() && Tab < int32(EDTTTab::Count); ++Tab)
    {
        GConfig->SetString(DTTHotkeySection, DTTTabIniNames[Tab], *TabKeys[Tab].ToString(), GGameUserSettingsIni);
    }
    GConfig->Flush(false, GGameUserSettingsIni);
}

void UDTTHotkeys::SetKeys(const FKey& Window, const TArray<FKey>& Tabs)
{
    WindowKey = Choices().Contains(Window) ? Window : EKeys::Invalid;
    TabKeys.Init(EKeys::Invalid, int32(EDTTTab::Count));
    for (int32 Tab = 0; Tab < TabKeys.Num() && Tab < Tabs.Num(); ++Tab)
    {
        TabKeys[Tab] = Choices().Contains(Tabs[Tab]) ? Tabs[Tab] : EKeys::Invalid;
    }
    Save();
    // The context is rebuilt in place: the game keeps its bookkeeping by handle, so the binding
    // is dropped and made again rather than edited under it.
    if (AFGPlayerController* PC = Controller.Get())
    {
        Uninstall();
        Install(PC);
    }
    UE_LOG(LogDTTHotkeys, Display, TEXT("HOTKEYS_SET Window=%s Tabs=%s"), *WindowKey.ToString(),
        *FString::JoinBy(TabKeys, TEXT(","), [](const FKey& K) { return K.ToString(); }));
}

void UDTTHotkeys::EnsureInstalled(UWorld* World)
{
    if (!IsValid(World) || World->GetNetMode() == NM_DedicatedServer) return;
    AFGPlayerController* PC = Cast<AFGPlayerController>(World->GetFirstPlayerController());
    if (!IsValid(PC) || !PC->IsLocalController()) return;
    if (Controller.Get() == PC && BoundHandle.IsValid()) return;
    // A remote client's controller gets its local player and input component a while after the
    // world has begun: until both are there, there is nothing to bind to.
    if (!PC->GetLocalPlayer() || !IsValid(PC->InputComponent)) return;
    if (Controller.IsValid() && Controller.Get() != PC) Uninstall();
    Install(PC);
}

void UDTTHotkeys::Install(AFGPlayerController* PC)
{
    if (!IsValid(PC)) return;
    if (TabKeys.Num() != int32(EDTTTab::Count)) Load();
    if (!WindowAction)
    {
        WindowAction = NewObject<UInputAction>(this, TEXT("DTT_Window"));
        WindowAction->ValueType = EInputActionValueType::Boolean;
    }
    while (TabActions.Num() < int32(EDTTTab::Count))
    {
        UInputAction* Action = NewObject<UInputAction>(this, *FString::Printf(TEXT("DTT_Tab%d"), TabActions.Num()));
        Action->ValueType = EInputActionValueType::Boolean;
        TabActions.Add(Action);
    }
    if (!Context) Context = NewObject<UInputMappingContext>(this, TEXT("DTT_Hotkeys"));
    MapKeys();

    UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PC->InputComponent);
    if (!Input)
    {
        UE_LOG(LogDTTHotkeys, Warning, TEXT("HOTKEYS_NO_INPUT_COMPONENT Class=%s"), *GetNameSafe(PC->InputComponent));
        return;
    }
    // The action bindings live on the controller's input component and survive a rebind of the
    // context; they are made once per controller.
    if (!bActionsBound || Controller.Get() != PC)
    {
        Input->ClearBindingsForObject(this);
        Input->BindAction(WindowAction, ETriggerEvent::Started, this, &UDTTHotkeys::HandleAction);
        for (UInputAction* Action : TabActions) Input->BindAction(Action, ETriggerEvent::Started, this, &UDTTHotkeys::HandleAction);
        bActionsBound = true;
    }
    Controller = PC;
    // One above the controller's own contexts, so a key of ours is not shadowed by a game
    // mapping of the same key - the game's default layout leaves the F keys alone.
    BoundHandle = PC->BindMappingContext(Context, PC->GetMappingContextPriority() + 1, this);

    // Only a warning: the player may well have put a game action on that key on purpose.
    if (UFGPlayerInput* PlayerInput = Cast<UFGPlayerInput>(PC->PlayerInput))
    {
        auto Warn = [&](const FKey& Key, const TCHAR* What)
        {
            if (!Key.IsValid()) return;
            TMap<const UInputMappingContext*, TArray<FEnhancedActionKeyMapping>> Taken;
            PlayerInput->FindKeysMappedTo(Key, {}, Taken);
            for (const auto& Pair : Taken)
            {
                if (Pair.Key == Context) continue;
                UE_LOG(LogDTTHotkeys, Warning, TEXT("HOTKEY_CONFLICT Key=%s Ours=%s GameContext=%s"), *Key.ToString(), What, *GetNameSafe(Pair.Key));
            }
        };
        Warn(WindowKey, TEXT("Window"));
        for (int32 Tab = 0; Tab < TabKeys.Num(); ++Tab) Warn(TabKeys[Tab], DTTTabIniNames[Tab]);
    }
    UE_LOG(LogDTTHotkeys, Display, TEXT("HOTKEYS_INSTALLED Window=%s Tabs=%s Priority=%d"), *WindowKey.ToString(),
        *FString::JoinBy(TabKeys, TEXT(","), [](const FKey& K) { return K.ToString(); }), PC->GetMappingContextPriority() + 1);
}

void UDTTHotkeys::Uninstall()
{
    if (AFGPlayerController* PC = Controller.Get())
    {
        if (BoundHandle.IsValid() && Context) PC->UnbindMappingContext(Context, BoundHandle);
    }
    BoundHandle.Reset();
}

void UDTTHotkeys::MapKeys()
{
    if (!Context) return;
    Context->UnmapAll();
    if (WindowKey.IsValid() && WindowAction) Context->MapKey(WindowAction, WindowKey);
    for (int32 Tab = 0; Tab < TabKeys.Num() && Tab < TabActions.Num(); ++Tab)
    {
        if (TabKeys[Tab].IsValid() && TabActions[Tab]) Context->MapKey(TabActions[Tab], TabKeys[Tab]);
    }
}

void UDTTHotkeys::HandleAction(const FInputActionInstance& Instance)
{
    AFGPlayerController* PC = Controller.Get();
    if (!IsValid(PC)) return;
    const UInputAction* Action = Instance.GetSourceAction();
    int32 Tab = -1;
    for (int32 Index = 0; Index < TabActions.Num(); ++Index) if (TabActions[Index] == Action) Tab = Index;
    if (Tab < 0 && Action != WindowAction) return;
    UDynamicTimeTableWidget::Toggle(PC, Tab);
}
