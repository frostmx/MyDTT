#pragma once

#include "CoreMinimal.h"
#include "UI/FGInteractWidget.h"
#include "DynamicTimeTableTypes.h"
#include "DTTRowButton.h"
#include "DynamicTimeTableWidget.generated.h"

class UBorder;
class UButton;
class UCheckBox;
class UComboBoxString;
class UEditableTextBox;
class UScrollBox;
class USizeBox;
class UProgressBar;
class UImage;
class UTexture2D;
class UTextBlock;
class UBorder;
class UWidget;
struct FDTTCardSink;
namespace DTTUI { struct FDTTStatusLine; }

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
    UPROPERTY() TArray<TObjectPtr<UImage>> Images;
    UPROPERTY() TArray<TObjectPtr<class UDTTRowButton>> Buttons;
    UPROPERTY() TArray<TObjectPtr<class UDTTRowCheck>> Checks;
};
class USpinBox;
class UTextBlock;
class UWidgetSwitcher;
class UDynamicTimeTableRemoteCallObject;

/**
 * The window's pages, in the order the switcher holds them. Every place that switches on the open
 * tab names one of these instead of a number: the page index, the tint of the tab button, the
 * snapshot section asked for and what a refresh redraws all have to agree, and they did so through
 * four separate literals that a new page would have shifted silently.
 */
enum class EDTTTab : int32
{
    Dispatch = 0,
    Trucks,
    Stations,
    Drones,
    Timetable,
    Groups,
    Settings,
    Count
};

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
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table")
    void RefreshData();

    /** EDTTSnapshotSection bits the open tab needs; the RCO's live poll asks for exactly these. */
    int32 GetRequestedSections() const;

private:
    UPROPERTY() TObjectPtr<UWidgetSwitcher> Pages;
    /** The window frame, resized from the viewport rather than nailed to one resolution. */
    UPROPERTY() TObjectPtr<USizeBox> WindowSizeBox;
    FVector2D AppliedWindowSize = FVector2D::ZeroVector;
    UPROPERTY() TObjectPtr<UButton> GroupsTabButton;
    UPROPERTY() TObjectPtr<UButton> DispatchTabButton;
    UPROPERTY() TObjectPtr<UButton> TrucksTabButton;
    UPROPERTY() TObjectPtr<UButton> StationsTabButton;
    UPROPERTY() TObjectPtr<UButton> DronesTabButton;
    UPROPERTY() TObjectPtr<UButton> TimetableTabButton;
    UPROPERTY() TObjectPtr<UButton> SettingsTabButton;
    // Settings page: one control per FDTTSettings field, filled from the server's copy on tab
    // open and after a result, never while the user is editing (bSettingsEdited).
    UPROPERTY() TObjectPtr<UCheckBox> SettingExactQuantities;
    UPROPERTY() TObjectPtr<USpinBox> SettingSolidRequest;
    UPROPERTY() TObjectPtr<USpinBox> SettingFluidRequest;
    UPROPERTY() TObjectPtr<USpinBox> SettingRequestBuffer;
    UPROPERTY() TObjectPtr<UCheckBox> SettingFluidFullLoad;
    UPROPERTY() TObjectPtr<UCheckBox> SettingDrainLeftovers;
    UPROPERTY() TObjectPtr<USpinBox> SettingLeftoverHold;
    UPROPERTY() TObjectPtr<UCheckBox> SettingStayUntilEmpty;
    UPROPERTY() TObjectPtr<USpinBox> SettingTopUpInterval;
    UPROPERTY() TObjectPtr<UCheckBox> SettingUnloadAtDepot;
    UPROPERTY() TObjectPtr<UCheckBox> SettingSkipProducer;
    UPROPERTY() TObjectPtr<USpinBox> SettingDispatchInterval;
    UPROPERTY() TObjectPtr<USpinBox> SettingMaxRequesters;
    UPROPERTY() TObjectPtr<USpinBox> SettingClaimTimeout;
    UPROPERTY() TObjectPtr<USpinBox> SettingOverdueMinutes;
    UPROPERTY() TObjectPtr<USpinBox> SettingIdleRecheck;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelCritical;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelSkip;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelLegMargin;
    UPROPERTY() TObjectPtr<USpinBox> SettingFuelReach;
    UPROPERTY() TObjectPtr<UCheckBox> SettingAutoName;
    UPROPERTY() TObjectPtr<USpinBox> SettingAutoNameDebounce;
    UPROPERTY() TObjectPtr<USpinBox> SettingAutoNameSweep;
    UPROPERTY() TObjectPtr<USpinBox> SettingAutoNameMaxNodes;
    UPROPERTY() TObjectPtr<USpinBox> SettingAutoNameMaxProducts;
    UPROPERTY() TObjectPtr<UComboBoxString> SettingNamingLanguage;
    /** Culture codes behind SettingNamingLanguage's options, in the same order. */
    TArray<FString> NamingLanguageCodes;
    UPROPERTY() TObjectPtr<UButton> SettingsSaveButton;
    UPROPERTY() TObjectPtr<UButton> SettingsDefaultsButton;
    UPROPERTY() TObjectPtr<UTextBlock> SettingsStatusText;
    bool bSettingsEdited = false;
    UPROPERTY() TObjectPtr<UScrollBox> DispatchScrollBox;
    UPROPERTY() TObjectPtr<UScrollBox> TruckScrollBox;
    UPROPERTY() TObjectPtr<UScrollBox> StationScrollBox;
    UPROPERTY() TObjectPtr<UScrollBox> DroneScrollBox;
    UPROPERTY() TObjectPtr<UScrollBox> TimetableScrollBox;
    /** The right-hand pane of the Timetable: open requests and the reasons nothing moves. */
    UPROPERTY() TObjectPtr<UScrollBox> QueueScrollBox;
    UPROPERTY() TArray<FDTTCardWidgets> QueueCards;
    UPROPERTY() TObjectPtr<UTextBlock> LapsPaneSummary;
    UPROPERTY() TObjectPtr<UTextBlock> QueuePaneSummary;
    /**
     * The four figures over the Timetable, built with the page and rewritten in place: a caption,
     * a big value, a line under it and a bar of up to four segments.
     */
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> KpiValues;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> KpiLines;
    UPROPERTY() TArray<TObjectPtr<UBorder>> KpiSegments;
    /** The status line under the tab row: a fixed row of text blocks, see DTTUI::FDTTStatusLine. */
    static constexpr int32 MaxStatusParts = 24;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> StatusParts;
    UPROPERTY() TObjectPtr<UEditableTextBox> DispatchFilter;
    UPROPERTY() TObjectPtr<UEditableTextBox> TruckFilter;
    UPROPERTY() TObjectPtr<UEditableTextBox> StationFilter;
    UPROPERTY() TObjectPtr<UEditableTextBox> DroneFilter;
    UPROPERTY() TObjectPtr<UEditableTextBox> TimetableFilter;
    // The Stations page edits the limit in one box above the table and writes it to every ticked
    // row. One box and not one per row: a spin box inside a card is recycled into another station
    // every refresh, and a half-typed number would be carried over to it or thrown away.
    UPROPERTY() TObjectPtr<USpinBox> StationLimitInput;
    UPROPERTY() TObjectPtr<UTextBlock> StationSelectionText;
    UPROPERTY() TObjectPtr<UButton> StationApplyButton;
    UPROPERTY() TObjectPtr<UButton> StationClearButton;
    /** Ticked rows, by dock guid. Survives a refresh; a row that disappears drops out silently. */
    TSet<FGuid> SelectedStations;
    // What the Stations table shows and in what order. The chips, the resource box and the
    // sortable captions are built once with the page and only relabelled afterwards.
    EDTTStationRoleFilter StationRoleFilter = EDTTStationRoleFilter::All;
    EDTTStationSort StationSort = EDTTStationSort::None;
    bool bStationSortDescending = false;
    /** The resource the box is set to; empty for all of them. */
    FString StationResourceFilter;
    UPROPERTY() TObjectPtr<UComboBoxString> StationResourceCombo;
    /** The resources the box offers now; the options are rebuilt only when this set changes. */
    TArray<FString> StationResourceOptions;
    /** Set while the options are being rebuilt, so the box's own selection event is not a choice. */
    bool bRebuildingStationResources = false;
    UPROPERTY() TArray<TObjectPtr<UButton>> StationChipButtons;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> StationChipCounts;
    /** The sortable captions, by EDTTStationSort, and the words they carry without an arrow. */
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> StationSortLabels;
    TArray<FString> StationSortCaptions;
    UPROPERTY() TObjectPtr<class UDTTRowCheck> StationSelectAllCheck;
    /** The requesters the table shows right now: what the tick over it selects. */
    TArray<FGuid> ShownStationRequesters;
    // The Dispatch and Trucks pages are cards; a card is rebuilt only when its own key changes
    // and updated in place otherwise, so the page never repaints as a whole.
    UPROPERTY() TArray<FDTTCardWidgets> DispatchCards;
    UPROPERTY() TArray<FDTTCardWidgets> TruckCards;
    UPROPERTY() TArray<FDTTCardWidgets> StationCards;
    UPROPERTY() TArray<FDTTCardWidgets> DroneCards;
    UPROPERTY() TArray<FDTTCardWidgets> TimetableCards;
    /** Item icons by descriptor path; a miss is cached too so an unresolvable class is not loaded again every refresh. */
    UPROPERTY() TMap<FSoftObjectPath, TObjectPtr<UTexture2D>> IconCache;
    UTexture2D* IconFor(const TSoftClassPtr<class UFGItemDescriptor>& ItemClass);
    /** The colour a truck carrying this item is painted in the world, for a row's tile. */
    FLinearColor TileColorFor(const TSoftClassPtr<class UFGItemDescriptor>& ItemClass);
    TMap<FSoftObjectPath, FLinearColor> TileColorCache;
    EDTTTab ActiveTab = EDTTTab::Dispatch;
    /**
     * The one open card of each page, by key, or empty for all closed. Part of the card's key,
     * so opening a row rebuilds that row and nothing else. The window is built afresh every
     * time it is opened (mSupportsCaching is off), so this does not outlive a close - and it
     * should not: a card left open over a session would hide the row the player came back for.
     */
    FString ExpandedDispatchKey;
    FString ExpandedTruckKey;
    FString ExpandedStationKey;
    FString ExpandedDroneKey;
    /**
     * An action that asks twice before it happens, and the card it belongs to. Only switching
     * an autopilot off needs this: it takes the truck out of the mod's hands altogether, and
     * everything else a row offers is undone by clicking something else. Cleared by any other
     * row action, so an armed button cannot sit waiting behind a page the player left.
     */
    EDTTRowAction ArmedAction = EDTTRowAction::None;
    FString ArmedKey;
    /** True while this control is the one waiting for its second click. */
    bool IsArmed(EDTTRowAction Action, const FString& CardId) const { return ArmedAction == Action && ArmedKey == CardId; }

    UPROPERTY() TObjectPtr<UScrollBox> GroupScrollBox;
    UPROPERTY() TObjectPtr<UTextBlock> SummaryText;
    UPROPERTY() TObjectPtr<UTextBlock> EditorStatusText;
    /** The footer line, the only status the player can read from every tab. */
    UPROPERTY() TObjectPtr<UTextBlock> StatusText;
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
    /** A share of the viewport within a ceiling; a no-op when the size has not changed. */
    void ApplyWindowSize();
    void BuildEditorPanel(class UVerticalBox* Main);
    void BuildSettingsPanel(class UVerticalBox* Page);
    /** The server's (or the snapshot's) settings into the controls. */
    void RefreshSettingsControls(bool bForce);
    FDTTSettings ReadSettingsControls() const;
    FDTTSettings ViewSettings() const;
    void CloseWindow();
    void RefreshRuntimeDisplay();
    void SetActiveTab(EDTTTab Tab);
    void RefreshDispatchDisplay();
    void RefreshTruckDisplay();
    void RefreshStationDisplay();
    void RefreshDroneDisplay();
    void RefreshTimetableDisplay();
    /** Writes the status line; the rest of its blocks collapse. Source is the right-hand note. */
    void SetStatusLine(const DTTUI::FDTTStatusLine& Line, const FString& Source);
    /** The Timetable's own layout: figures over two panes, not the filter-and-table of the others. */
    class UVerticalBox* BuildTimetablePage();
    /** One figure of the four: returns nothing, stores its widgets in the Kpi arrays. */
    void BuildKpiTile(class UHorizontalBox* Row, const FString& Caption);
    void SetKpi(int32 Index, const FString& Value, const FLinearColor& ValueColour, const FString& Line,
        const TArray<TPair<float, FLinearColor>>& Segments);
    void SyncCards(UScrollBox* Page, TArray<FDTTCardWidgets>& Cards, const TArray<FString>& Keys,
        TFunctionRef<void(int32, FDTTCardSink&)> Fill);
    /** Bound to HandleRowAction once and handed to every sink, which binds it to every control. */
    FDTTRowActionDelegate RowActionDelegate;
    void PopulateResourceCard(const FString& Resource, const TArray<const FDTTDispatchRow*>& Rows,
        const FString& CardId, bool bExpanded, FDTTCardSink& Sink);
    void PopulateDepotCard(const TArray<const FDTTDispatchRow*>& Depots, FDTTCardSink& Sink);
    void PopulateFuelCard(const TArray<const FDTTDispatchRow*>& FuelStops, FDTTCardSink& Sink);
    void PopulateTruckCard(const FDTTTruckRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink);
    void PopulateStationCard(const FDTTStationRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink);
    void PopulateDroneCard(const FDTTDroneRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink);
    /** LapWidth is the room the lap line has, which decides how many stops it draws. */
    void PopulateLapCard(const FDTTLapRow& Row, float LapWidth, FDTTCardSink& Sink);
    void PopulateQueueCard(const FDTTQueueRow& Row, FDTTCardSink& Sink);
    /** "Why nothing moves": one chip per reason, with the skips of the last pass summed over resources. */
    void PopulateReasonCard(const TArray<FDTTReasonRow>& Totals, FDTTCardSink& Sink);
    /** A group heading of the Timetable: its title, a rule and a count. */
    void PopulateSectionCard(const FString& Title, const FLinearColor& Colour, const FString& Count, FDTTCardSink& Sink);
    /** The station rows the window is looking at: the snapshot's on a client, the subsystem's on a host. */
    void GatherStationRows(TArray<FDTTStationRow>& OutRows) const;
    /** The ticked docks, as handles, for an operation that acts on the selection. */
    void GatherSelectedDocks(TArray<class AFGDockingStationIdentifier*>& OutDocks) const;
    /** The captions over a table's columns, built once with the page. */
    void BuildDispatchHeader(class UHorizontalBox* Row);
    void BuildTruckHeader(class UHorizontalBox* Row);
    void BuildStationHeader(class UHorizontalBox* Row);
    void BuildDroneHeader(class UHorizontalBox* Row);
    void BuildTimetableHeader(class UHorizontalBox* Row);
    /** The limit box and the two buttons that act on the ticked rows. */
    void BuildStationTools(class UHorizontalBox* Row);
    void RebuildRuntimeDisplay();
    void UpdateRuntimeDisplayInPlace();
    bool RuntimeLayoutMatches() const;
    void RefreshEditorControls();
    void SetEditorStatus(const FString& Message, bool bError = false);
    /** Every row control of every page arrives here; see FDTTRowTarget. */
    void HandleRowAction(const FDTTRowTarget& Target);
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
    class UVerticalBox* BuildCardPage(TObjectPtr<UScrollBox>& OutScroll, TObjectPtr<UEditableTextBox>& OutFilter,
        class UHorizontalBox*& OutHeader, class UHorizontalBox** OutTools = nullptr, class UHorizontalBox** OutFilterRow = nullptr);
    /** The role chips and the resource box of the Stations page, after its text filter. */
    void BuildStationFilters(class UHorizontalBox* Row);
    UFUNCTION() void HandleStationResourceChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
    UFUNCTION() void HandleTrucksTabClicked();
    UFUNCTION() void HandleStationsTabClicked();
    UFUNCTION() void HandleDronesTabClicked();
    UFUNCTION() void HandleDroneFilterChanged(const FText& Text);
    UFUNCTION() void HandleTimetableTabClicked();
    UFUNCTION() void HandleTimetableFilterChanged(const FText& Text);
    UFUNCTION() void HandleStationFilterChanged(const FText& Text);
    /** The two page-level buttons of the Stations tab; they act on the ticked rows, not on one. */
    UFUNCTION() void HandleStationApplyClicked();
    UFUNCTION() void HandleStationClearClicked();
    UFUNCTION() void HandleSettingsTabClicked();
    UFUNCTION() void HandleSettingsSaveClicked();
    UFUNCTION() void HandleSettingsDefaultsClicked();
    UFUNCTION() void HandleSettingSpinChanged(float Value);
    UFUNCTION() void HandleSettingCheckChanged(bool bChecked);
    UFUNCTION() void HandleSettingComboChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
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
