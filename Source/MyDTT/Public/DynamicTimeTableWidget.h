#pragma once

#include "CoreMinimal.h"
#include "UI/FGInteractWidget.h"
#include "DynamicTimeTableWidget.generated.h"

class UBorder;
class UButton;
class UCheckBox;
class UComboBoxString;
class UEditableTextBox;
class UScrollBox;
class UProgressBar;
class UTextBlock;
class UBorder;
class UWidget;
struct FDTTCardSink;

/** One card of the Dispatch or Trucks page: its root in the scroll box and its widgets in creation order. */
USTRUCT()
struct FDTTCardWidgets
{
    GENERATED_BODY()

    FString Key;
    UPROPERTY() TObjectPtr<UWidget> Root;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> Texts;
    UPROPERTY() TArray<TObjectPtr<UBorder>> Borders;
    UPROPERTY() TArray<TObjectPtr<UProgressBar>> Bars;
};
class USpinBox;
class UTextBlock;
class UWidgetSwitcher;
class UDynamicTimeTableRemoteCallObject;

UCLASS(NotBlueprintable)
class MYDTT_API UDynamicTimeTableWidget : public UFGInteractWidget
{
    GENERATED_BODY()

public:
    UDynamicTimeTableWidget(const FObjectInitializer& ObjectInitializer);

    virtual void NativeOnInitialized() override;
    virtual void NativeConstruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
    virtual void NativeDestruct() override;
    virtual void OnEscapePressed_Implementation() override;

    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table")
    void RefreshData();

    /** EDTTSnapshotSection bits the open tab needs; the RCO's live poll asks for exactly these. */
    int32 GetRequestedSections() const;

private:
    // Tabs: 0 groups (editor + booking status), 1 dispatch stations, 2 trucks, 3 settings.
    UPROPERTY() TObjectPtr<UWidgetSwitcher> Pages;
    UPROPERTY() TObjectPtr<UButton> GroupsTabButton;
    UPROPERTY() TObjectPtr<UButton> DispatchTabButton;
    UPROPERTY() TObjectPtr<UButton> TrucksTabButton;
    UPROPERTY() TObjectPtr<UButton> SettingsTabButton;
    // Settings page: one control per FDTTSettings field, filled from the server's copy on tab
    // open and after a result, never while the user is editing (bSettingsEdited).
    UPROPERTY() TObjectPtr<USpinBox> SettingSolidRequest;
    UPROPERTY() TObjectPtr<USpinBox> SettingFluidRequest;
    UPROPERTY() TObjectPtr<USpinBox> SettingRequestBuffer;
    UPROPERTY() TObjectPtr<UCheckBox> SettingFluidFullLoad;
    UPROPERTY() TObjectPtr<UCheckBox> SettingDrainLeftovers;
    UPROPERTY() TObjectPtr<USpinBox> SettingLeftoverHold;
    UPROPERTY() TObjectPtr<USpinBox> SettingTopUpInterval;
    UPROPERTY() TObjectPtr<UCheckBox> SettingUnloadAtDepot;
    UPROPERTY() TObjectPtr<UCheckBox> SettingSkipProducer;
    UPROPERTY() TObjectPtr<USpinBox> SettingDispatchInterval;
    UPROPERTY() TObjectPtr<USpinBox> SettingMaxRequesters;
    UPROPERTY() TObjectPtr<USpinBox> SettingClaimTimeout;
    UPROPERTY() TObjectPtr<USpinBox> SettingIdleRecheck;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelCritical;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelSkip;
    UPROPERTY() TObjectPtr<UButton> SettingsSaveButton;
    UPROPERTY() TObjectPtr<UButton> SettingsDefaultsButton;
    UPROPERTY() TObjectPtr<UTextBlock> SettingsStatusText;
    bool bSettingsEdited = false;
    UPROPERTY() TObjectPtr<UScrollBox> DispatchScrollBox;
    UPROPERTY() TObjectPtr<UScrollBox> TruckScrollBox;
    UPROPERTY() TObjectPtr<UEditableTextBox> DispatchFilter;
    UPROPERTY() TObjectPtr<UEditableTextBox> TruckFilter;
    // The Dispatch and Trucks pages are cards; a card is rebuilt only when its own key changes
    // and updated in place otherwise, so the page never repaints as a whole.
    UPROPERTY() TArray<FDTTCardWidgets> DispatchCards;
    UPROPERTY() TArray<FDTTCardWidgets> TruckCards;
    int32 ActiveTab = 0;

    UPROPERTY() TObjectPtr<UScrollBox> GroupScrollBox;
    UPROPERTY() TObjectPtr<UTextBlock> SummaryText;
    UPROPERTY() TObjectPtr<UTextBlock> EditorStatusText;
    UPROPERTY() TObjectPtr<UButton> CloseButton;
    UPROPERTY() TObjectPtr<UButton> RecalculateButton;

    UPROPERTY() TObjectPtr<UComboBoxString> GroupSelector;
    UPROPERTY() TObjectPtr<UEditableTextBox> GroupNameInput;
    UPROPERTY() TObjectPtr<UEditableTextBox> NewGroupNameInput;
    UPROPERTY() TObjectPtr<UComboBoxString> NewGroupKindSelector;
    UPROPERTY() TObjectPtr<UButton> CreateGroupButton;
    UPROPERTY() TObjectPtr<UButton> RenameGroupButton;
    UPROPERTY() TObjectPtr<UButton> DeleteGroupButton;

    UPROPERTY() TObjectPtr<UComboBoxString> AvailableStationSelector;
    UPROPERTY() TObjectPtr<UComboBoxString> GroupStationSelector;
    UPROPERTY() TObjectPtr<USpinBox> TargetSlotsInput;
    UPROPERTY() TObjectPtr<UButton> AddStationButton;
    UPROPERTY() TObjectPtr<UButton> ApplySlotsButton;
    UPROPERTY() TObjectPtr<UButton> RemoveStationButton;

    TArray<FGuid> GroupOptionIds;
    FGuid SelectedGroupId;
    FDelegateHandle GroupsChangedHandle;
    FDelegateHandle SnapshotChangedHandle;
    FDelegateHandle OperationResultHandle;
    UPROPERTY() TObjectPtr<UDynamicTimeTableRemoteCallObject> RemoteCallObject;
    UPROPERTY() TArray<TObjectPtr<UBorder>> RuntimeGroupBorders;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> RuntimeGroupTexts;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> RuntimeStationTexts;
    TArray<FGuid> RuntimeGroupIds;
    TArray<int32> RuntimeStationCounts;
    TArray<FString> RuntimeStationNames;
    bool bInitialFocusApplied = false;
    bool bRefreshingControls = false;
    bool bHasAppliedRemoteConfiguration = false;
    bool bForceFullRefreshOnNextSnapshot = false;
    int32 LastAppliedConfigurationRevision = INDEX_NONE;
    float LocalLiveRefreshAccumulator = 0.0f;

    void BuildWidgetTree();
    void BuildEditorPanel(class UVerticalBox* Main);
    void BuildSettingsPanel(class UVerticalBox* Page);
    /** The server's (or the snapshot's) settings into the controls. */
    void RefreshSettingsControls(bool bForce);
    FDTTSettings ReadSettingsControls() const;
    FDTTSettings ViewSettings() const;
    void CloseWindow();
    void RefreshRuntimeDisplay();
    void SetActiveTab(int32 Tab);
    void RefreshDispatchDisplay();
    void RefreshTruckDisplay();
    void SyncCards(UScrollBox* Page, TArray<FDTTCardWidgets>& Cards, const TArray<FString>& Keys,
        TFunctionRef<void(int32, FDTTCardSink&)> Fill);
    void PopulateResourceCard(const FString& Resource, const TArray<const FDTTDispatchRow*>& Rows, FDTTCardSink& Sink);
    void PopulateDepotCard(const TArray<const FDTTDispatchRow*>& Depots, FDTTCardSink& Sink);
    void PopulateFuelCard(const TArray<const FDTTDispatchRow*>& FuelStops, FDTTCardSink& Sink);
    void PopulateTruckCard(const FDTTTruckRow& Row, FDTTCardSink& Sink);
    void RebuildRuntimeDisplay();
    void UpdateRuntimeDisplayInPlace();
    bool RuntimeLayoutMatches() const;
    void RefreshEditorControls();
    void SetEditorStatus(const FString& Message, bool bError = false);
    FString GetSelectedGroupStationName() const;
    bool IsRemoteClient() const;
    const TArray<FDTTStationGroup>& ViewGroups() const;
    void HandleLocalGroupsChanged();
    void HandleRemoteSnapshot();
    void HandleRemoteResult(bool bSuccess, const FString& Message);

    UFUNCTION() void HandleCloseClicked();
    UFUNCTION() void HandleGroupsTabClicked();
    UFUNCTION() void HandleDispatchTabClicked();
    UFUNCTION() void HandleDispatchFilterChanged(const FText& Text);
    UFUNCTION() void HandleTruckFilterChanged(const FText& Text);
    /** A page of cards: the filter line above a scroll box that fills the rest. */
    class UVerticalBox* BuildCardPage(TObjectPtr<UScrollBox>& OutScroll, TObjectPtr<UEditableTextBox>& OutFilter);
    UFUNCTION() void HandleTrucksTabClicked();
    UFUNCTION() void HandleSettingsTabClicked();
    UFUNCTION() void HandleSettingsSaveClicked();
    UFUNCTION() void HandleSettingsDefaultsClicked();
    UFUNCTION() void HandleSettingSpinChanged(float Value);
    UFUNCTION() void HandleSettingCheckChanged(bool bChecked);
    UFUNCTION() void HandleRecalculateClicked();
    UFUNCTION() void HandleCreateGroupClicked();
    UFUNCTION() void HandleRenameGroupClicked();
    UFUNCTION() void HandleDeleteGroupClicked();
    UFUNCTION() void HandleAddStationClicked();
    UFUNCTION() void HandleApplySlotsClicked();
    UFUNCTION() void HandleRemoveStationClicked();
    UFUNCTION() void HandleGroupSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
    UFUNCTION() void HandleGroupStationSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
};
