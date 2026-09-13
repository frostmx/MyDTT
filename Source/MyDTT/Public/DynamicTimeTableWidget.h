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
struct FDTTCardSink;
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
    // The Dispatch and Trucks pages are cards, built when the set of rows changes and updated in
    // place otherwise; the arrays hold the widgets in creation order, the keys say what was built.
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> DispatchCardTexts;
    UPROPERTY() TArray<TObjectPtr<UBorder>> DispatchCardBorders;
    UPROPERTY() TArray<TObjectPtr<UProgressBar>> DispatchCardBars;
    TArray<FString> DispatchCardKeys;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> TruckCardTexts;
    UPROPERTY() TArray<TObjectPtr<UBorder>> TruckCardBorders;
    UPROPERTY() TArray<TObjectPtr<UProgressBar>> TruckCardBars;
    TArray<FString> TruckCardKeys;
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
    void PopulateDispatchCards(const TArray<FDTTDispatchRow>& Rows, FDTTCardSink& Sink);
    void PopulateTruckCards(const TArray<FDTTTruckRow>& Rows, FDTTCardSink& Sink);
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
