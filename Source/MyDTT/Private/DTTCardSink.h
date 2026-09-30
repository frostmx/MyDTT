#pragma once

// The window's drawing kit: the card sink, the palette and the small text helpers every page
// shares. Split out of DynamicTimeTableWidget.cpp, which was the whole window in one file.

#include "CoreMinimal.h"
#include "DynamicTimeTableWidget.h"
#include "DTTRowButton.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/SpinBox.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"

class UTexture2D;

DECLARE_LOG_CATEGORY_EXTERN(LogDynamicTimeTableWidget, Log, All);

namespace DTTUI
{
    /**
     * The design document's colours, written as the sRGB hex it gives them. UMG colours are linear,
     * so every one goes through FromSRGBColor: a hex typed in as a linear literal comes out a
     * washed-out grey, which is what the window looked like before.
     */
    inline FLinearColor Hex(uint32 RGB, float Alpha = 1.0f)
    {
        FLinearColor Colour = FLinearColor::FromSRGBColor(FColor((RGB >> 16) & 0xFF, (RGB >> 8) & 0xFF, RGB & 0xFF));
        Colour.A = Alpha;
        return Colour;
    }

    // Surfaces, darkest first.
    inline const FLinearColor DTTWindowBg = Hex(0x23313a);
    inline const FLinearColor DTTWindowLine = Hex(0x4d6775);
    inline const FLinearColor DTTTopBar = Hex(0x2b3c47);
    inline const FLinearColor DTTSubBar = Hex(0x27363f);
    inline const FLinearColor DTTPane = Hex(0x2a3a45);
    inline const FLinearColor DTTPaneHead = Hex(0x2e404b);
    inline const FLinearColor DTTTrack = Hex(0x3f545f);
    inline const FLinearColor DTTTabOn = Hex(0x40616f);
    // A row, and a row that says something: waiting for a decision, overdue, taken.
    inline const FLinearColor DTTCardTint = Hex(0x2f414d);
    inline const FLinearColor DTTCardTintWait = Hex(0x3a3830);
    inline const FLinearColor DTTCardTintWarn = Hex(0x3d2f2f);
    inline const FLinearColor DTTCardTintOpen = Hex(0x2c4540);

    // What a colour means - the legend, one for one.
    /** Now: the stop being driven to, the open card, the value being edited. */
    inline const FLinearColor DTTCyan = Hex(0x4fc9e8);
    /** In hand: a request somebody took, a dock above its line. */
    inline const FLinearColor DTTGreen = Hex(0x5fd39b);
    /** Waiting for a decision: open and unclaimed, autopilot off. */
    inline const FLinearColor DTTYellow = Hex(0xffc857);
    /** Broken or overdue. */
    inline const FLinearColor DTTRed = Hex(0xff8a8a);
    /** The pool: depots, parked trucks. */
    inline const FLinearColor DTTSteel = Hex(0xa3b5e8);
    /** Fuel. */
    inline const FLinearColor DTTOrange = Hex(0xffa45c);
    /** Quiet on purpose. Most of a healthy factory. */
    inline const FLinearColor DTTDim = Hex(0x8296a3);
    /** Secondary text. */
    inline const FLinearColor DTTGrey = Hex(0xb8c8d2);
    inline const FLinearColor DTTText = Hex(0xf0f6f9);
    /**
     * A dock above its line. Green, as the legend has it - and not grey, which on a stock bar reads
     * as empty: that was a complaint once already.
     */
    inline const FLinearColor DTTStocked = DTTGreen;

    /** What a thing is, as a small label: a badge says what, colour says how it is doing. */
    enum class EDTTPill : uint8
    {
        Producer, Requester, Depot, Fuel, Mode, ModeWrong,
        PoolAssigned, PoolParked, PoolToDepot, Manual, Docked, AutopilotOff, Dispatch,
    };

    /** The legend's badge pairs, background and text. */
    inline void PillColours(EDTTPill Kind, FLinearColor& OutBack, FLinearColor& OutText)
    {
        uint32 Back = 0x3a4b57, Fore = 0xcfdde5;
        switch (Kind)
        {
        case EDTTPill::Producer: Back = 0x22423a; Fore = 0xbdf0dd; break;
        case EDTTPill::Requester: Back = 0x4a3a22; Fore = 0xffe0b0; break;
        case EDTTPill::Depot: Back = 0x313d5c; Fore = 0xd3dcf7; break;
        case EDTTPill::Fuel: Back = 0x4d3520; Fore = 0xffd9b0; break;
        case EDTTPill::Mode: Back = 0x2f5364; Fore = 0xcfeaf5; break;
        case EDTTPill::ModeWrong: Back = 0x5c2f2f; Fore = 0xffd0d0; break;
        case EDTTPill::PoolAssigned: Back = 0x2b4c66; Fore = 0xcfeaf5; break;
        case EDTTPill::PoolParked: Back = 0x3a4b57; Fore = 0xcfdde5; break;
        case EDTTPill::PoolToDepot: Back = 0x313d5c; Fore = 0xd3dcf7; break;
        case EDTTPill::Manual: Back = 0x5c4423; Fore = 0xffe9c6; break;
        case EDTTPill::Docked: Back = 0x2f5364; Fore = 0xcfeaf5; break;
        case EDTTPill::AutopilotOff: Back = 0x5c2f2f; Fore = 0xffd0d0; break;
        case EDTTPill::Dispatch: Back = 0x33464f; Fore = 0xcfdde5; break;
        }
        OutBack = Hex(Back);
        OutText = Hex(Fore);
    }

    inline UTextBlock* MakeText(UWidgetTree* Tree, const FString& Value, int32 Size, const FLinearColor& Color)
    {
        UTextBlock* Text = Tree->ConstructWidget<UTextBlock>();
        Text->SetText(FText::FromString(Value));
        Text->SetColorAndOpacity(FSlateColor(Color));
        FSlateFontInfo Font = Text->GetFont();
        Font.Size = Size;
        Text->SetFont(Font);
        return Text;
    }

    /** A solid rounded rectangle, which is a brush with no texture behind it. */
    inline FSlateBrush FlatBrush(const FLinearColor& Tint, float Radius = 3.0f)
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
        Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
        Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
        Brush.TintColor = FSlateColor(Tint);
        return Brush;
    }

    /**
     * Makes a button look like part of a table rather than like a game button: flat, square-ish
     * and with padding of our own. The tint is left to SetBackgroundColor, which multiplies
     * every state at once, so the row can recolour itself on an update pass.
     */
    inline void Flatten(UButton* Button, const FMargin& Padding, bool bRow = false)
    {
        // A row draws nothing of its own - the card's tint is already behind it - and only
        // lights up under the pointer, which is the one thing that says a row can be clicked.
        const float Normal = bRow ? 0.0f : 0.10f;
        FButtonStyle Style = Button->GetStyle();
        Style.SetNormal(FlatBrush(FLinearColor(1.0f, 1.0f, 1.0f, Normal)));
        Style.SetHovered(FlatBrush(FLinearColor(1.0f, 1.0f, 1.0f, bRow ? 0.10f : 0.20f)));
        Style.SetPressed(FlatBrush(FLinearColor(1.0f, 1.0f, 1.0f, bRow ? 0.16f : 0.30f)));
        Style.SetDisabled(FlatBrush(FLinearColor(1.0f, 1.0f, 1.0f, Normal * 0.4f)));
        Style.SetNormalPadding(Padding);
        Style.SetPressedPadding(Padding);
        Button->SetStyle(Style);
    }

    /** A card tint one step brighter, for the row that is open. */
    inline FLinearColor Lighten(const FLinearColor& Tint, float By = 0.05f)
    {
        return FLinearColor(Tint.R + By, Tint.G + By, Tint.B + By, Tint.A);
    }

    /**
     * A progress bar the way this window wants one: a dark track so that any fill reads as fill,
     * and a plain white fill image for SetFillColorAndOpacity to tint. The stock engine style is
     * a light track under a light fill, and at a glance a full station looked like an empty one.
     */
    inline void StyleBar(UProgressBar* Bar)
    {
        FProgressBarStyle Style = Bar->GetWidgetStyle();
        Style.BackgroundImage = FlatBrush(FLinearColor(0.02f, 0.03f, 0.04f, 0.85f), 2.0f);
        Style.FillImage = FlatBrush(FLinearColor::White, 2.0f);
        Bar->SetWidgetStyle(Style);
    }

    /** A rounded box with its own outline, and per-corner radii for a bar that caps a panel. */
    inline FSlateBrush OutlinedBrush(const FLinearColor& Fill, const FLinearColor& Line, float Radius, float Width = 1.0f)
    {
        FSlateBrush Brush = FlatBrush(Fill, Radius);
        Brush.OutlineSettings.Color = FSlateColor(Line);
        Brush.OutlineSettings.Width = Width;
        return Brush;
    }
    inline FSlateBrush CornerBrush(const FLinearColor& Fill, const FVector4& Radii)
    {
        FSlateBrush Brush = FlatBrush(Fill, 0.0f);
        Brush.OutlineSettings.CornerRadii = Radii;
        return Brush;
    }

    /**
     * The design's buttons. Ghost is every ordinary button: outlined, no fill until the pointer is
     * on it. Go is the one action a panel exists for. Tab and TabOn are the tab row. The engine's
     * default button is a light bevelled slab that belongs to no window of this game.
     */
    enum class EDTTButton : uint8 { Ghost, Go, Tab, TabOn };

    inline void StyleButton(UButton* Button, EDTTButton Kind)
    {
        if (!Button) return;
        const FLinearColor None(0.0f, 0.0f, 0.0f, 0.0f);
        FButtonStyle Style = Button->GetStyle();
        FLinearColor TextColour = Hex(0xcfdde5);
        FMargin Padding(11, 4);
        switch (Kind)
        {
        case EDTTButton::Go:
            Style.SetNormal(OutlinedBrush(Hex(0x2f5b52), Hex(0x4a8878), 6.0f));
            Style.SetHovered(OutlinedBrush(Hex(0x3a6d62), Hex(0x5a9a8a), 6.0f));
            Style.SetPressed(OutlinedBrush(Hex(0x28504a), Hex(0x4a8878), 6.0f));
            TextColour = Hex(0xe4fbf4);
            break;
        case EDTTButton::Tab:
        case EDTTButton::TabOn:
        {
            const FLinearColor Fill = Kind == EDTTButton::TabOn ? DTTTabOn : None;
            Style.SetNormal(FlatBrush(Fill, 7.0f));
            Style.SetHovered(FlatBrush(Kind == EDTTButton::TabOn ? DTTTabOn : Hex(0x38505c), 7.0f));
            Style.SetPressed(FlatBrush(DTTTabOn, 7.0f));
            TextColour = Kind == EDTTButton::TabOn ? FLinearColor::White : DTTGrey;
            Padding = FMargin(13, 6);
            break;
        }
        default:
            Style.SetNormal(OutlinedBrush(None, DTTWindowLine, 6.0f));
            Style.SetHovered(OutlinedBrush(Hex(0x38505c), DTTWindowLine, 6.0f));
            Style.SetPressed(OutlinedBrush(Hex(0x2f4550), DTTWindowLine, 6.0f));
            break;
        }
        Style.SetDisabled(OutlinedBrush(None, Hex(0x3a4f5b), 6.0f));
        Style.SetNormalPadding(Padding);
        Style.SetPressedPadding(Padding);
        Button->SetStyle(Style);
        Button->SetBackgroundColor(FLinearColor::White);
        if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent())) Label->SetColorAndOpacity(FSlateColor(TextColour));
    }

    inline UButton* MakeButton(UWidgetTree* Tree, const FString& Label, EDTTButton Kind = EDTTButton::Ghost)
    {
        UButton* Button = Tree->ConstructWidget<UButton>();
        const bool bTab = Kind == EDTTButton::Tab || Kind == EDTTButton::TabOn;
        Button->SetContent(MakeText(Tree, Label, bTab ? 15 : 13, FLinearColor::White));
        StyleButton(Button, Kind);
        return Button;
    }

    /**
     * The status line under the tab row: counts in white or in their meaning's colour, words in
     * grey. Built once per refresh and handed to the window, which has a fixed set of text blocks
     * for it - the line is never rebuilt, only rewritten.
     */
    struct FDTTStatusLine
    {
        /** A middle dot, spelled as a code point: the sources stay ASCII. */
        static FString Separator() { return FString(TEXT("   ")) + FString::Chr(0x00B7) + TEXT("   "); }
        TArray<TPair<FString, FLinearColor>> Parts;
        FDTTStatusLine& Count(int32 Value, const FString& Label, const FLinearColor& Colour = DTTText)
        {
            if (Parts.Num() > 0) Parts.Emplace(Separator(), DTTDim);
            Parts.Emplace(FString::FromInt(Value), Colour);
            Parts.Emplace(TEXT(" ") + Label, DTTGrey);
            return *this;
        }
        FDTTStatusLine& Words(const FString& Value, const FLinearColor& Colour = DTTGrey)
        {
            if (Parts.Num() > 0) Parts.Emplace(Separator(), DTTDim);
            Parts.Emplace(Value, Colour);
            return *this;
        }
    };

    inline UHorizontalBoxSlot* AddRowItem(UHorizontalBox* Row, UWidget* Widget, float Padding = 5.0f, bool bFill = false)
    {
        UHorizontalBoxSlot* Slot = Row->AddChildToHorizontalBox(Widget);
        Slot->SetPadding(FMargin(Padding));
        Slot->SetVerticalAlignment(VAlign_Center);
        if (bFill) Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        return Slot;
    }

    // Column widths of the two tables, in slate units, shared by the rows and the header above
    // them: a caption that does not sit over its column is worse than no caption. One column of
    // each row is left to fill, so every other one is fixed.
    inline constexpr float DTTRowHeight = 40.0f;
    inline constexpr float DTTColIcon = 20.0f;
    inline constexpr float DTTColResource = 226.0f;
    inline constexpr float DTTColProducers = 190.0f;
    inline constexpr float DTTColState = 172.0f;
    inline constexpr float DTTColWait = 62.0f;
    inline constexpr float DTTColChevron = 20.0f;
    inline constexpr float DTTColTruck = 232.0f;
    inline constexpr float DTTColPhase = 124.0f;
    inline constexpr float DTTColCargo = 108.0f;
    // Stations table.
    inline constexpr float DTTColCheck = 26.0f;
    inline constexpr float DTTColItem = 132.0f;
    inline constexpr float DTTColGroup = 104.0f;
    inline constexpr float DTTColRole = 78.0f;
    /** Wide enough for the UNLOAD pill; narrower and it lies over the stock bar. */
    inline constexpr float DTTColMode = 74.0f;
    inline constexpr float DTTColStock = 62.0f;
    inline constexpr float DTTColLimit = 48.0f;
    inline constexpr float DTTColStationTile = 22.0f;
    /** The cargo tile of a Dispatch or Trucks row. */
    inline constexpr float DTTColRowTile = 24.0f;
    /** One station's own bar, wider than the eight that share a resource row. */
    inline constexpr float DTTStockBarWide = 96.0f;
    // Timetable.
    inline constexpr float DTTColLeft = 86.0f;
    inline constexpr float DTTColTile = 28.0f;
    /** Resource over destination: wider than the Trucks column, which puts the two on one line. */
    inline constexpr float DTTColLapWho = 260.0f;
    inline constexpr float DTTColSpeed = 64.0f;
    inline constexpr float DTTColNext = 56.0f;
    inline constexpr float DTTColLapLeft = 76.0f;
    inline constexpr float DTTColLapCargo = 92.0f;
    /** The laps pane against the requests pane. */
    inline constexpr float DTTLapsPaneShare = 2.2f;
    /** One stop of a lap. Fixed, or a three-stop lap would draw a bar across the window. */
    inline constexpr float DTTStripMark = 26.0f;
    /** One requester's bar inside a resource row; eight of them fit before the row gives up. */
    inline constexpr float DTTStockBarWidth = 54.0f;
    inline constexpr int32 DTTMaxStockBars = 8;

    /**
     * The design's text box: dark, outlined, cyan when it has the focus. The engine's is a white
     * slab, and its hint text is light grey on it - there to be read by nobody.
     */
    inline void StyleInput(UEditableTextBox* Box, int32 FontSize = 13)
    {
        if (!Box) return;
        FEditableTextBoxStyle& Style = Box->WidgetStyle;
        FSlateFontInfo Font = Style.TextStyle.Font;
        Font.Size = FontSize;
        const FLinearColor Fill = Hex(0x1f2b33);
        Style.SetFont(Font).SetPadding(FMargin(9, 4))
            .SetBackgroundImageNormal(OutlinedBrush(Fill, Hex(0x44606e), 7.0f))
            .SetBackgroundImageHovered(OutlinedBrush(Fill, Hex(0x5d8494), 7.0f))
            .SetBackgroundImageFocused(OutlinedBrush(Fill, DTTCyan, 7.0f))
            .SetBackgroundImageReadOnly(OutlinedBrush(Fill, Hex(0x3a4f5b), 7.0f))
            .SetForegroundColor(FSlateColor(DTTText))
            .SetFocusedForegroundColor(FSlateColor(DTTText))
            .SetBackgroundColor(FSlateColor(FLinearColor::White));
    }

    /** A filter chip: rounded, outlined, filled when it is the one in force. */
    inline void StyleChip(UButton* Button, bool bOn)
    {
        if (!Button) return;
        FButtonStyle Style = Button->GetStyle();
        const FLinearColor Fill = bOn ? DTTTabOn : Hex(0x2c3d48);
        const FLinearColor Line = bOn ? Hex(0x5d8494) : Hex(0x44606e);
        Style.SetNormal(OutlinedBrush(Fill, Line, 14.0f));
        Style.SetHovered(OutlinedBrush(Lighten(Fill, 0.04f), Hex(0x5d8494), 14.0f));
        Style.SetPressed(OutlinedBrush(Lighten(Fill, 0.08f), Hex(0x5d8494), 14.0f));
        Style.SetDisabled(OutlinedBrush(Fill, Line, 14.0f));
        Style.SetNormalPadding(FMargin(11, 3));
        Style.SetPressedPadding(FMargin(11, 3));
        Button->SetStyle(Style);
    }

    /** A 14-point box: outlined when clear, cyan when ticked. The stock one is dark on dark. */
    inline void StyleCheck(UCheckBox* Check)
    {
        if (!Check) return;
        auto Box = [](const FLinearColor& Fill, const FLinearColor& Line)
        {
            FSlateBrush Brush = OutlinedBrush(Fill, Line, 3.0f);
            Brush.ImageSize = FVector2D(14.0f, 14.0f);
            return Brush;
        };
        FCheckBoxStyle Style = Check->GetWidgetStyle();
        Style.SetCheckBoxType(ESlateCheckBoxType::CheckBox);
        Style.SetUncheckedImage(Box(Hex(0x1f2b33), Hex(0x6a8795)));
        Style.SetUncheckedHoveredImage(Box(Hex(0x2c3d48), DTTCyan));
        Style.SetUncheckedPressedImage(Box(Hex(0x2c3d48), DTTCyan));
        Style.SetCheckedImage(Box(DTTCyan, DTTCyan));
        Style.SetCheckedHoveredImage(Box(Lighten(DTTCyan, 0.08f), DTTCyan));
        Style.SetCheckedPressedImage(Box(DTTCyan, DTTCyan));
        Style.SetUndeterminedImage(Box(Hex(0x2f5364), DTTCyan));
        Style.SetUndeterminedHoveredImage(Box(Hex(0x2f5364), DTTCyan));
        Style.SetUndeterminedPressedImage(Box(Hex(0x2f5364), DTTCyan));
        Style.SetPadding(FMargin(0));
        Check->SetWidgetStyle(Style);
    }

    /** The resource box: a ghost button that opens a list. Font and colour only take at construction. */
    inline void StyleCombo(UComboBoxString* Combo, int32 FontSize = 13)
    {
        if (!Combo) return;
        FComboBoxStyle Style = Combo->GetWidgetStyle();
        FButtonStyle& Button = Style.ComboButtonStyle.ButtonStyle;
        const FLinearColor Fill = Hex(0x2c3d48);
        Button.SetNormal(OutlinedBrush(Fill, Hex(0x44606e), 7.0f));
        Button.SetHovered(OutlinedBrush(Lighten(Fill, 0.04f), Hex(0x5d8494), 7.0f));
        Button.SetPressed(OutlinedBrush(Lighten(Fill, 0.08f), Hex(0x5d8494), 7.0f));
        Button.SetNormalPadding(FMargin(10, 3));
        Button.SetPressedPadding(FMargin(10, 3));
        Style.ComboButtonStyle.MenuBorderBrush = FlatBrush(DTTTopBar, 6.0f);
        Combo->SetWidgetStyle(Style);
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        Combo->Font.Size = FontSize;
        Combo->ForegroundColor = FSlateColor(DTTText);
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }

    /** A number box in the design's colours; the engine's is a light slab like its text box. */
    inline void StyleSpin(USpinBox* Spin, int32 FontSize = 13)
    {
        if (!Spin) return;
        FSpinBoxStyle Style = Spin->GetWidgetStyle();
        const FLinearColor Fill = Hex(0x1f2b33);
        Style.SetBackgroundBrush(OutlinedBrush(Fill, Hex(0x44606e), 6.0f))
            .SetHoveredBackgroundBrush(OutlinedBrush(Fill, Hex(0x5d8494), 6.0f))
            .SetActiveBackgroundBrush(OutlinedBrush(Fill, DTTCyan, 6.0f))
            .SetInactiveFillBrush(FlatBrush(Hex(0x26353f), 6.0f))
            .SetHoveredFillBrush(FlatBrush(Hex(0x2f4653), 6.0f))
            .SetActiveFillBrush(FlatBrush(Hex(0x2f5364), 6.0f))
            .SetForegroundColor(FSlateColor(DTTText));
        Spin->SetWidgetStyle(Style);
        Spin->SetForegroundColor(FSlateColor(DTTText));
        FSlateFontInfo Font = Spin->GetFont();
        Font.Size = FontSize;
        Spin->SetFont(Font);
    }

    /** A caption over a column, the same width as the Cell that will stand under it. */
    inline void HeadCell(UWidgetTree* Tree, UHorizontalBox* Row, const FString& Caption, float Width,
        EHorizontalAlignment Align = HAlign_Left, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        UTextBlock* Text = MakeText(Tree, Caption, 11, FLinearColor(0.72f, 0.78f, 0.82f));
        USizeBox* Box = Tree->ConstructWidget<USizeBox>();
        if (Width > 0.0f) Box->SetWidthOverride(Width);
        Box->SetContent(Text);
        if (USizeBoxSlot* S = Cast<USizeBoxSlot>(Text->Slot)) { S->SetHorizontalAlignment(Align); S->SetVerticalAlignment(VAlign_Center); }
        Row->AddChild(Box);
        if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
        {
            S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
            if (Width <= 0.0f) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        }
    }

    /**
     * HeadCell as a button: the same box, width and margin, so the caption stands exactly where
     * the plain one did, and a click on it carries Target. Returns the caption to relabel.
     */
    inline UTextBlock* HeadButton(UWidgetTree* Tree, UHorizontalBox* Row, const FString& Caption, float Width,
        EHorizontalAlignment Align, const FMargin& Margin, const FDTTRowActionDelegate& Action, const FDTTRowTarget& Target)
    {
        UDTTRowButton* Button = Tree->ConstructWidget<UDTTRowButton>();
        Button->Bind(Action);
        Button->Attach(Target);
        Flatten(Button, FMargin(0), true);
        UTextBlock* Text = MakeText(Tree, Caption, 11, FLinearColor(0.72f, 0.78f, 0.82f));
        Button->SetContent(Text);
        if (UButtonSlot* S = Cast<UButtonSlot>(Text->Slot)) { S->SetPadding(FMargin(0)); S->SetHorizontalAlignment(Align); S->SetVerticalAlignment(VAlign_Center); }
        USizeBox* Box = Tree->ConstructWidget<USizeBox>();
        if (Width > 0.0f) Box->SetWidthOverride(Width);
        Box->SetContent(Button);
        Row->AddChild(Box);
        if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
        {
            S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
            if (Width <= 0.0f) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        }
        return Text;
    }

}

// A card is built once and then updated in place on every refresh: the sink hands out the card's
// stored widgets in creation order, so the walk that fills a card must produce the same shape
// for a build and for an update. The card key covers what decides the widget count; a key
// change rebuilds that one card, the rest of the page stays as it is (no flicker, no scroll
// jump). Text that is empty collapses, so optional lines are always emitted.
struct FDTTCardSink
{
    UWidgetTree* Tree = nullptr;
    bool bBuild = true;
    FDTTCardWidgets& Card;
    int32 TextIndex = 0, BorderIndex = 0, BarIndex = 0, ImageIndex = 0, ButtonIndex = 0, CheckIndex = 0;
    /** What every control this sink makes reports to; see UDynamicTimeTableWidget::HandleRowAction. */
    FDTTRowActionDelegate RowAction;

    FDTTCardSink(UWidgetTree* InTree, bool bInBuild, FDTTCardWidgets& InCard) : Tree(InTree), bBuild(bInBuild), Card(InCard) {}

    /** True when an update walked past what the build stored: the shape changed without the key noticing. */
    bool Overran() const { return TextIndex != Card.Texts.Num() || BorderIndex != Card.Borders.Num() || BarIndex != Card.Bars.Num() || ImageIndex != Card.Images.Num() || ButtonIndex != Card.Buttons.Num() || CheckIndex != Card.Checks.Num(); }

    template <typename TSlot>
    static void Pad(UWidget* Widget, const FMargin& Margin)
    {
        if (Widget) if (TSlot* S = Cast<TSlot>(Widget->Slot)) S->SetPadding(Margin);
    }

    UVerticalBox* VBox(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
        if (Parent) { Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin); }
        return Box;
    }

    UHorizontalBox* HBox(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UHorizontalBox* Box = Tree->ConstructWidget<UHorizontalBox>();
        if (Parent) { Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin); }
        return Box;
    }

    /** A column of a two-column body: fills its share, aligned to the top. */
    UVerticalBox* Column(UHorizontalBox* Row)
    {
        if (!bBuild) return nullptr;
        UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
        if (Row)
        {
            UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Box);
            S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S->SetPadding(FMargin(0, 0, 10, 0)); S->SetVerticalAlignment(VAlign_Top);
        }
        return Box;
    }

    UWrapBox* Wrap(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UWrapBox* Box = Tree->ConstructWidget<UWrapBox>();
        Box->SetInnerSlotPadding(FVector2D(4, 2));
        if (Parent)
        {
            Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin);
            // In a row the wrap box must take the remaining width, or it wraps after every chip.
            if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S->SetVerticalAlignment(VAlign_Center); }
        }
        return Box;
    }

    UTextBlock* Text(UPanelWidget* Parent, const FString& Value, int32 Size, const FLinearColor& Color,
        bool bWrap = false, const FMargin& Margin = FMargin(0), bool bFill = false)
    {
        UTextBlock* T = nullptr;
        if (bBuild)
        {
            T = DTTUI::MakeText(Tree, Value, Size, Color);
            if (bWrap) T->SetAutoWrapText(true);
            if (Parent)
            {
                Parent->AddChild(T);
                Pad<UVerticalBoxSlot>(T, Margin); Pad<UWrapBoxSlot>(T, Margin);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(T->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    if (bFill) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
            Card.Texts.Add(T);
        }
        else if (Card.Texts.IsValidIndex(TextIndex))
        {
            T = Card.Texts[TextIndex].Get();
            if (T) { T->SetText(FText::FromString(Value)); T->SetColorAndOpacity(FSlateColor(Color)); }
        }
        if (T) T->SetVisibility(Value.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
        ++TextIndex;
        return T;
    }

    /**
     * The card itself: a tinted border with a vertical body, stored as the card's root. Returns the
     * body (null on an update). A card that is one row of a table passes no padding: the row has
     * its own fixed height, and a border around it would make the table twice as tall.
     */
    UVerticalBox* Root(const FLinearColor& Tint, const FMargin& Padding = FMargin(12, 8))
    {
        UVerticalBox* Body = nullptr;
        if (bBuild)
        {
            UBorder* Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrush(DTTUI::FlatBrush(FLinearColor::White, 6.0f));
            Border->SetBrushColor(Tint); Border->SetPadding(Padding);
            Body = Tree->ConstructWidget<UVerticalBox>(); Border->SetContent(Body);
            Card.Root = Border;
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            if (UBorder* Border = Card.Borders[BorderIndex].Get()) Border->SetBrushColor(Tint);
        }
        ++BorderIndex;
        return Body;
    }

    /** A small coloured label: "Pool · Parked", "Autopilot off". Hidden ones keep their place. */
    void Badge(UPanelWidget* Parent, const FString& Label, const FLinearColor& Background, bool bVisible = true, const FLinearColor& Foreground = FLinearColor::White)
    {
        UBorder* Border = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrush(DTTUI::FlatBrush(FLinearColor::White, 4.0f));
            Border->SetBrushColor(Background); Border->SetPadding(FMargin(7, 1));
            if (Parent)
            {
                Parent->AddChild(Border);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Border->Slot)) { S->SetPadding(FMargin(0, 0, 6, 0)); S->SetVerticalAlignment(VAlign_Center); }
            }
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Border = Card.Borders[BorderIndex].Get();
            if (Border) Border->SetBrushColor(Background);
        }
        ++BorderIndex;
        UTextBlock* T = Text(nullptr, Label, 12, Foreground);
        if (bBuild && Border && T) Border->SetContent(T);
        if (Border) Border->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }

    /**
     * A badge standing in a column of a table: the column keeps its width whatever the label, and a
     * hidden pill still holds its place, so the columns after it stay under their captions.
     */
    void Pill(UPanelWidget* Parent, const FString& Label, DTTUI::EDTTPill Kind, float Width,
        bool bVisible = true, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        FLinearColor Back, Fore;
        DTTUI::PillColours(Kind, Back, Fore);
        UBorder* Border = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrush(DTTUI::FlatBrush(FLinearColor::White, 4.0f));
            Border->SetPadding(FMargin(7, 1));
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            Box->SetContent(Border);
            if (USizeBoxSlot* S = Cast<USizeBoxSlot>(Border->Slot)) { S->SetHorizontalAlignment(HAlign_Left); S->SetVerticalAlignment(VAlign_Center); }
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Border = Card.Borders[BorderIndex].Get();
        }
        ++BorderIndex;
        UTextBlock* T = Text(nullptr, Label, 11, Fore);
        if (bBuild && Border && T) Border->SetContent(T);
        if (!Border) return;
        Border->SetBrushColor(Back);
        Border->SetVisibility(bVisible && !Label.IsEmpty() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Hidden);
    }

    /**
     * The cargo tile of the design: a rounded square in the colour the truck is painted for this
     * cargo in the world, with the item's icon on it. The colour is what ties a row in the window to
     * a truck on the road. It always keeps its place; with nothing to show it is just the colour.
     */
    void Tile(UPanelWidget* Parent, UTexture2D* Texture, const FLinearColor& Colour, float Size,
        const FMargin& Margin = FMargin(0, 0, 8, 0))
    {
        UBorder* Border = nullptr;
        UImage* Image = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrush(DTTUI::FlatBrush(FLinearColor::White, 5.0f));
            Border->SetPadding(FMargin(3));
            Image = Tree->ConstructWidget<UImage>();
            Border->SetContent(Image);
            if (UBorderSlot* S = Cast<UBorderSlot>(Image->Slot)) { S->SetHorizontalAlignment(HAlign_Fill); S->SetVerticalAlignment(VAlign_Fill); }
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetWidthOverride(Size); Box->SetHeightOverride(Size); Box->SetContent(Border);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Borders.Add(Border);
            Card.Images.Add(Image);
        }
        else
        {
            if (Card.Borders.IsValidIndex(BorderIndex)) Border = Card.Borders[BorderIndex].Get();
            if (Card.Images.IsValidIndex(ImageIndex)) Image = Card.Images[ImageIndex].Get();
        }
        ++BorderIndex;
        ++ImageIndex;
        if (Border) Border->SetBrushColor(Colour);
        if (Image)
        {
            if (Texture) Image->SetBrushFromTexture(Texture, false);
            Image->SetVisibility(Texture ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Hidden);
        }
    }

    /** An item icon of Size points; collapsed when there is no texture, keeping its place. */
    void Icon(UPanelWidget* Parent, UTexture2D* Texture, float Size, const FMargin& Margin = FMargin(0, 0, 6, 0),
        bool bKeepSpace = false)
    {
        UImage* Image = nullptr;
        if (bBuild)
        {
            Image = Tree->ConstructWidget<UImage>();
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetWidthOverride(Size); Box->SetHeightOverride(Size); Box->SetContent(Image);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Images.Add(Image);
        }
        else if (Card.Images.IsValidIndex(ImageIndex))
        {
            Image = Card.Images[ImageIndex].Get();
        }
        ++ImageIndex;
        if (!Image) return;
        if (Texture) Image->SetBrushFromTexture(Texture, false);
        // The size box is what takes the space, so hide that. In a table the column has to stay
        // even when the item has no icon, so there it is hidden rather than collapsed.
        UWidget* Holder = Image->GetParent() ? static_cast<UWidget*>(Image->GetParent()) : static_cast<UWidget*>(Image);
        Holder->SetVisibility(Texture ? ESlateVisibility::SelfHitTestInvisible
            : (bKeepSpace ? ESlateVisibility::Hidden : ESlateVisibility::Collapsed));
    }

    /** A thin fill bar; Percent 0..1. */
    void Bar(UPanelWidget* Parent, float Percent, const FLinearColor& Color, const FMargin& Margin = FMargin(0, 3, 0, 2))
    {
        Percent = FMath::Clamp(Percent, 0.0f, 1.0f);
        if (bBuild)
        {
            UProgressBar* B = Tree->ConstructWidget<UProgressBar>();
            DTTUI::StyleBar(B);
            B->SetPercent(Percent); B->SetFillColorAndOpacity(Color);
            USizeBox* Size = Tree->ConstructWidget<USizeBox>(); Size->SetHeightOverride(7); Size->SetContent(B);
            if (Parent) { Parent->AddChild(Size); Pad<UVerticalBoxSlot>(Size, Margin); }
            Card.Bars.Add(B);
        }
        else if (Card.Bars.IsValidIndex(BarIndex))
        {
            if (UProgressBar* B = Card.Bars[BarIndex].Get()) { B->SetPercent(Percent); B->SetFillColorAndOpacity(Color); }
        }
        ++BarIndex;
    }
    /**
     * One line of a table, and the line itself is the control: a click anywhere on it carries
     * Target, which is how a row opens. Returns the box to fill, null on an update like every other
     * container here. Height is fixed so that the columns of every row line up.
     */
    UHorizontalBox* Row(UPanelWidget* Parent, float Height, const FDTTRowTarget& Target,
        const FLinearColor& Tint, const FMargin& Margin = FMargin(0, 1))
    {
        UDTTRowButton* B = nullptr;
        UHorizontalBox* Line = nullptr;
        if (bBuild)
        {
            B = Tree->ConstructWidget<UDTTRowButton>();
            B->Bind(RowAction);
            DTTUI::Flatten(B, FMargin(6, 0), true);
            Line = Tree->ConstructWidget<UHorizontalBox>();
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetHeightOverride(Height); Box->SetContent(Line);
            B->SetContent(Box);
            // A button centres its content, which would leave every column of the table
            // floating in the middle of the window instead of under its caption.
            if (UButtonSlot* S = Cast<UButtonSlot>(Box->Slot))
            {
                S->SetHorizontalAlignment(HAlign_Fill);
                S->SetVerticalAlignment(VAlign_Fill);
                S->SetPadding(FMargin(0));
            }
            if (Parent) { Parent->AddChild(B); Pad<UVerticalBoxSlot>(B, Margin); }
            Card.Buttons.Add(B);
        }
        else if (Card.Buttons.IsValidIndex(ButtonIndex))
        {
            B = Card.Buttons[ButtonIndex].Get();
        }
        ++ButtonIndex;
        // Every pass, build and update alike: a recycled widget stands in a different row now.
        if (B) { B->Attach(Target); B->SetBackgroundColor(Tint); }
        return Line;
    }

    /**
     * A column of a Row: fixed width so the columns line up, or Width 0 to take what is left.
     * bWrap lets the text run onto the following lines instead of ending in an ellipsis; the row
     * has to be tall enough for them, the box clips whatever still does not fit.
     */
    UTextBlock* Cell(UPanelWidget* Line, const FString& Value, int32 Size, const FLinearColor& Color,
        float Width, EHorizontalAlignment Align = HAlign_Left, const FMargin& Margin = FMargin(0, 0, 6, 0),
        bool bWrap = false)
    {
        UTextBlock* T = nullptr;
        if (bBuild)
        {
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            T = Text(nullptr, Value, Size, Color, bWrap);
            // A size box holds a column's width but does not stop the text inside it from drawing
            // past that: a long station name printed straight over the next column. The ellipsis
            // says the name goes on, which a clean cut would not.
            if (!bWrap) T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
            Box->SetClipping(EWidgetClipping::ClipToBounds);
            Box->SetContent(T);
            // The text fills its column and justifies itself inside it. Aligning the slot
            // instead would hand the text its own desired width, which is exactly the width
            // the column is there to limit, and the ellipsis would never appear.
            T->SetJustification(Align == HAlign_Right ? ETextJustify::Right
                : (Align == HAlign_Center ? ETextJustify::Center : ETextJustify::Left));
            if (USizeBoxSlot* S = Cast<USizeBoxSlot>(T->Slot)) { S->SetHorizontalAlignment(HAlign_Fill); S->SetVerticalAlignment(VAlign_Center); }
            if (Line)
            {
                Line->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    if (Width <= 0.0f) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
            KeepColumn(T, Width);
            return T;
        }
        T = Text(nullptr, Value, Size, Color);
        KeepColumn(T, Width);
        return T;
    }

    /**
     * An empty cell still owns its column. A size box whose child is collapsed reports no size
     * at all - the width override does not survive a collapsed child - and the row's fill column
     * swallows the difference, so one station without a group pushed every column after it out
     * from under its caption. Empty text draws nothing either way; only the layout differs.
     */
    static void KeepColumn(UTextBlock* Text, float Width)
    {
        if (Text && Width > 0.0f && Text->GetVisibility() == ESlateVisibility::Collapsed)
        {
            Text->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    /**
     * A column of two lines: what a thing is, and under it one line about it. Fixed width, both
     * lines ellipsised - the column is the width, never the text.
     */
    void Cell2(UPanelWidget* Line, const FString& Top, int32 TopSize, const FLinearColor& TopColour,
        const FString& Bottom, int32 BottomSize, const FLinearColor& BottomColour, float Width,
        const FMargin& Margin = FMargin(0, 0, 8, 0), EHorizontalAlignment Align = HAlign_Left)
    {
        UVerticalBox* Stack = nullptr;
        if (bBuild)
        {
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            Box->SetClipping(EWidgetClipping::ClipToBounds);
            Stack = Tree->ConstructWidget<UVerticalBox>();
            Box->SetContent(Stack);
            if (USizeBoxSlot* S = Cast<USizeBoxSlot>(Stack->Slot)) { S->SetHorizontalAlignment(HAlign_Fill); S->SetVerticalAlignment(VAlign_Center); }
            if (Line)
            {
                Line->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    if (Width <= 0.0f) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
        }
        UTextBlock* A = Text(Stack, Top, TopSize, TopColour);
        UTextBlock* B = Text(Stack, Bottom, BottomSize, BottomColour);
        if (bBuild)
        {
            for (UTextBlock* T : { A, B })
            {
                if (!T) continue;
                T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
                T->SetJustification(Align == HAlign_Right ? ETextJustify::Right : ETextJustify::Left);
            }
        }
        // An empty pair still owns its column; see KeepColumn.
        KeepColumn(A, Width);
    }

    /** A number with a thin bar under it, right-aligned in a fixed column: "26 / 64 st". */
    void CellBar(UPanelWidget* Line, const FString& Value, const FLinearColor& Colour, float Percent,
        const FLinearColor& BarColour, float Width, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        UVerticalBox* Stack = nullptr;
        if (bBuild)
        {
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetWidthOverride(Width);
            Stack = Tree->ConstructWidget<UVerticalBox>();
            Box->SetContent(Stack);
            if (USizeBoxSlot* S = Cast<USizeBoxSlot>(Stack->Slot)) { S->SetHorizontalAlignment(HAlign_Fill); S->SetVerticalAlignment(VAlign_Center); }
            if (Line)
            {
                Line->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
            }
        }
        UTextBlock* T = Text(Stack, Value, 12, Colour);
        if (bBuild && T) T->SetJustification(ETextJustify::Right);
        KeepColumn(T, Width);
        Bar(Stack, Percent, BarColour, FMargin(0, 3, 0, 0));
    }

    /** A 1-point line across whatever width the row leaves it, for a group heading. */
    void Rule(UPanelWidget* Line, const FLinearColor& Colour, const FMargin& Margin = FMargin(8, 0))
    {
        UBorder* Border = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            // A plain image brush: a rounded box one point high comes out as nothing at all.
            FSlateBrush Plain;
            Plain.TintColor = FSlateColor(FLinearColor::White);
            Border->SetBrush(Plain);
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetHeightOverride(1.0f); Box->SetContent(Border);
            if (Line)
            {
                Line->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Border = Card.Borders[BorderIndex].Get();
        }
        ++BorderIndex;
        if (Border) Border->SetBrushColor(Colour);
    }

    /**
     * One stop of a lap drawn across the lap column: its name, at most MaxWidth wide, and - when
     * bLeg - the leg that follows it, a 3-point rule that takes whatever width is left, with
     * LegLabel over it. Always emits the same widgets so the shape does not depend on where the
     * truck is; an empty Name collapses the stop. MaxWidth is applied on every pass, because it
     * follows the width the window gives the column.
     */
    void Stop(UPanelWidget* Line, const FString& Name, const FLinearColor& NameColour, float MaxWidth,
        bool bLeg, const FLinearColor& LegColour, const FString& LegLabel = FString())
    {
        UTextBlock* T = nullptr;
        if (bBuild)
        {
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetClipping(EWidgetClipping::ClipToBounds);
            T = Text(nullptr, Name, 12, NameColour);
            T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
            Box->SetContent(T);
            if (Line)
            {
                Line->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetVerticalAlignment(VAlign_Center); }
            }
        }
        else
        {
            T = Text(nullptr, Name, 12, NameColour);
        }
        if (T) if (USizeBox* Box = Cast<USizeBox>(T->GetParent())) Box->SetMaxDesiredWidth(MaxWidth);

        // The leg: its label over the rule, and as much room under the rule as the label takes
        // over it, so the rule stays on the row's middle line whether the label shows or not.
        USizeBox* Leg = nullptr;
        UVerticalBox* Stack = nullptr;
        if (bBuild)
        {
            Leg = Tree->ConstructWidget<USizeBox>();
            Leg->SetMinDesiredWidth(24.0f);
            Stack = Tree->ConstructWidget<UVerticalBox>();
            Leg->SetContent(Stack);
            if (Line)
            {
                Line->AddChild(Leg);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Leg->Slot))
                {
                    S->SetPadding(FMargin(8, 0)); S->SetVerticalAlignment(VAlign_Center);
                    S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
        }
        UTextBlock* Label = Text(Stack, LegLabel, 10, DTTUI::DTTGrey);
        if (bBuild && Label)
        {
            Label->SetJustification(ETextJustify::Center);
            Pad<UVerticalBoxSlot>(Label, FMargin(0, 0, 0, 1));
        }
        if (Label && LegLabel.IsEmpty()) Label->SetVisibility(ESlateVisibility::Hidden);
        UBorder* Rule = nullptr;
        if (bBuild)
        {
            Rule = Tree->ConstructWidget<UBorder>();
            Rule->SetBrush(DTTUI::FlatBrush(FLinearColor::White, 1.5f));
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetHeightOverride(3.0f); Box->SetContent(Rule);
            Stack->AddChild(Box);
            Pad<UVerticalBoxSlot>(Box, FMargin(0, 0, 0, 14));
            Card.Borders.Add(Rule);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Rule = Card.Borders[BorderIndex].Get();
        }
        ++BorderIndex;
        if (!Rule) return;
        Rule->SetBrushColor(LegColour);
        // Rule -> its size box -> the stack -> the leg's own size box, which is what takes the room.
        UWidget* Holder = Rule;
        for (int32 Up = 0; Up < 3 && Holder->GetParent(); ++Up) Holder = Holder->GetParent();
        Holder->SetVisibility(bLeg ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }

    /** A rounded chip with a label and a count in amber: "no free truck  6". */
    void Chip(UPanelWidget* Parent, const FString& Label, const FString& Count)
    {
        UBorder* Border = nullptr;
        UHorizontalBox* Line = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrush(DTTUI::OutlinedBrush(DTTUI::Hex(0x354a55), DTTUI::Hex(0x445c69), 12.0f));
            Border->SetPadding(FMargin(10, 3));
            Line = Tree->ConstructWidget<UHorizontalBox>();
            Border->SetContent(Line);
            if (Parent) Parent->AddChild(Border);
            Card.Borders.Add(Border);
        }
        ++BorderIndex;
        Text(Line, Label, 12, DTTUI::Hex(0xdce8ee), false, FMargin(0, 0, 6, 0));
        Text(Line, Count, 12, DTTUI::DTTYellow);
    }

    /**
     * A control in a row. Never leave the call out on a condition: pass bVisible and let it
     * collapse, or the card changes shape with its state and a recycled button ends up pointed at
     * a row that is no longer there.
     */
    UDTTRowButton* Button(UPanelWidget* Parent, const FString& Label, const FDTTRowTarget& Target,
        bool bVisible = true, bool bEnabled = true, float Width = 0.0f,
        const FMargin& Margin = FMargin(0, 0, 4, 0))
    {
        UDTTRowButton* B = nullptr;
        if (bBuild)
        {
            B = Tree->ConstructWidget<UDTTRowButton>();
            B->Bind(RowAction);
            DTTUI::Flatten(B, FMargin(8, 2));
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            Box->SetContent(B);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Buttons.Add(B);
        }
        else if (Card.Buttons.IsValidIndex(ButtonIndex))
        {
            B = Card.Buttons[ButtonIndex].Get();
        }
        ++ButtonIndex;
        UTextBlock* T = Text(nullptr, Label, 12, FLinearColor::White);
        if (bBuild && B && T) B->SetContent(T);
        if (!B) return nullptr;
        B->Attach(Target);
        B->SetIsEnabled(bEnabled);
        // The size box is what takes the space in the row, so that is what hides.
        UWidget* Holder = B->GetParent() ? static_cast<UWidget*>(B->GetParent()) : static_cast<UWidget*>(B);
        Holder->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
        return B;
    }

    /**
     * A tick box in a row, for a selection the page holds. Same discipline as Button: never leave
     * the call out on a condition. bChecked is written on every pass, because the box is recycled
     * into a different row and would otherwise keep the last row's tick.
     */
    UDTTRowCheck* Check(UPanelWidget* Parent, bool bChecked, const FDTTRowTarget& Target,
        bool bVisible = true, float Width = 0.0f, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        UDTTRowCheck* C = nullptr;
        if (bBuild)
        {
            C = Tree->ConstructWidget<UDTTRowCheck>();
            C->Bind(RowAction);
            DTTUI::StyleCheck(C);
            // A box of the column's width, so that the caption above it and every row below
            // stand in the same place: a check is whatever size the style makes it otherwise.
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            Box->SetContent(C);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Checks.Add(C);
        }
        else if (Card.Checks.IsValidIndex(CheckIndex))
        {
            C = Card.Checks[CheckIndex].Get();
        }
        ++CheckIndex;
        if (!C) return nullptr;
        C->Attach(Target);
        // SetIsChecked does not raise OnCheckStateChanged - that fires on a real toggle only -
        // so writing the state back here cannot be mistaken for the player ticking the box.
        if (C->IsChecked() != bChecked) C->SetIsChecked(bChecked);
        // Hidden, not collapsed: the column has to keep its width in every row.
        C->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
        return C;
    }

    /**
     * The stock of one station as a bar with the two levels marked on it: where it starts calling
     * and what a delivery fills it to. Three numbers in prose is what the cards said before, and
     * nobody could see at a glance which stations were about to call.
     *
     * The width has to be fixed: a marker is placed by the left padding of its slot, and in a bar
     * that fills its row there is no width to place it against.
     */
    void MarkedBar(UPanelWidget* Parent, float Fill, float Trigger, float Target,
        const FLinearColor& FillColor, float Width, float Height = 10.0f,
        const FMargin& Margin = FMargin(0, 0, 8, 0))
    {
        UOverlay* Stack = nullptr;
        if (bBuild)
        {
            Stack = Tree->ConstructWidget<UOverlay>();
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetWidthOverride(Width); Box->SetHeightOverride(Height); Box->SetContent(Stack);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                // A vertical box stretches its children across the column, and the width override
                // does not survive that: the bar came out the width of the card while the marks
                // were still placed for Width, and the call line of a station at 88 % stood at 25 %.
                if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetHorizontalAlignment(HAlign_Left); }
            }
        }
        UProgressBar* B = nullptr;
        if (bBuild)
        {
            B = Tree->ConstructWidget<UProgressBar>();
            DTTUI::StyleBar(B);
            if (UOverlaySlot* S = Stack->AddChildToOverlay(B)) { S->SetHorizontalAlignment(HAlign_Fill); S->SetVerticalAlignment(VAlign_Fill); }
            Card.Bars.Add(B);
        }
        else if (Card.Bars.IsValidIndex(BarIndex))
        {
            B = Card.Bars[BarIndex].Get();
        }
        ++BarIndex;
        if (B) { B->SetPercent(FMath::Clamp(Fill, 0.0f, 1.0f)); B->SetFillColorAndOpacity(FillColor); }
        BarMark(Stack, Trigger, Width, DTTUI::DTTYellow);
        BarMark(Stack, Target, Width, DTTUI::DTTCyan);
    }

    /**
     * A lap drawn as its stops: one mark each, the one the truck is driving to lit and the rest
     * dim. Where a truck is in its lap is a glance this way; as a list of station names it was a
     * line of text nobody read. The stop count belongs in the card's key - it decides how many
     * marks there are, and a card that changes shape without its key noticing is the one failure
     * this sink cannot see.
     */
    void Strip(UPanelWidget* Parent, int32 Stops, int32 Current, const FLinearColor& Colour,
        float Height = 12.0f, const FMargin& Margin = FMargin(0, 0, 8, 0))
    {
        UHorizontalBox* Line = nullptr;
        if (bBuild)
        {
            Line = Tree->ConstructWidget<UHorizontalBox>();
            if (Parent)
            {
                Parent->AddChild(Line);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Line->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
        }
        const FLinearColor Dimmed(Colour.R * 0.35f, Colour.G * 0.35f, Colour.B * 0.35f, 0.9f);
        for (int32 Index = 0; Index < Stops; ++Index)
        {
            UBorder* Mark = nullptr;
            if (bBuild)
            {
                Mark = Tree->ConstructWidget<UBorder>();
                USizeBox* Box = Tree->ConstructWidget<USizeBox>();
                Box->SetHeightOverride(Height); Box->SetContent(Mark);
                Box->SetWidthOverride(DTTUI::DTTStripMark);
                if (Line)
                {
                    Line->AddChild(Box);
                    if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot))
                    {
                        S->SetPadding(FMargin(0, 0, 3, 0)); S->SetVerticalAlignment(VAlign_Center);
                    }
                }
                Card.Borders.Add(Mark);
            }
            else if (Card.Borders.IsValidIndex(BorderIndex))
            {
                Mark = Card.Borders[BorderIndex].Get();
            }
            ++BorderIndex;
            if (Mark) Mark->SetBrushColor(Index == Current ? Colour : Dimmed);
        }
    }

    /** One 2-point line across a MarkedBar. A level outside 0..1 has nothing to mark, so it hides. */
    void BarMark(UOverlay* Stack, float At, float Width, const FLinearColor& Color)
    {
        UBorder* Mark = nullptr;
        if (bBuild)
        {
            Mark = Tree->ConstructWidget<UBorder>();
            USizeBox* Thin = Tree->ConstructWidget<USizeBox>();
            Thin->SetWidthOverride(2.0f); Thin->SetContent(Mark);
            if (Stack)
            {
                if (UOverlaySlot* S = Stack->AddChildToOverlay(Thin)) { S->SetHorizontalAlignment(HAlign_Left); S->SetVerticalAlignment(VAlign_Fill); }
            }
            Card.Borders.Add(Mark);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Mark = Card.Borders[BorderIndex].Get();
        }
        ++BorderIndex;
        if (!Mark) return;
        Mark->SetBrushColor(Color);
        UWidget* Holder = Mark->GetParent() ? static_cast<UWidget*>(Mark->GetParent()) : static_cast<UWidget*>(Mark);
        const bool bVisible = At >= 0.0f && At <= 1.0f;
        Pad<UOverlaySlot>(Holder, FMargin(At * FMath::Max(0.0f, Width - 2.0f), 0, 0, 0));
        Holder->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }
};

namespace DTTUI
{
    inline FString DTTStacks(int32 Stacks) { return Stacks >= 0 ? FString::Printf(TEXT("%d stacks"), Stacks) : FString(TEXT("unknown")); }
    /** A station named by its tag alone ("(Coal)") has an empty base name. */
    inline FString DTTStationName(const FString& Name) { return Name.IsEmpty() ? FString(TEXT("(no name)")) : Name; }
    /** The identifier number the subsystem appends to a truck's display name ("Ore -> Smelter#2147123023"): the display part changes with every target. */
    inline FString DTTTruckKey(const FString& Name)
    {
        int32 Hash = INDEX_NONE;
        return Name.FindLastChar(TEXT('#'), Hash) ? Name.Mid(Hash + 1) : Name;
    }
    /** The name without the object number: the number tells /dtt why which truck, the card does not need it. */
    inline FString DTTTruckTitle(const FString& Name)
    {
        int32 Hash = INDEX_NONE;
        return Name.FindLastChar(TEXT('#'), Hash) ? Name.Left(Hash) : Name;
    }
    /** The mod names a dispatch truck "<resource> -> <dock>", so the head would print the resource
        twice and the dock a second time after the route line: that name carries nothing the card
        does not already show. A name the player chose stays as the head's right-hand summary. */
    inline FString DTTTruckTail(const FString& Title, const FString& Resource)
    {
        if (Resource.IsEmpty() || !Title.StartsWith(Resource, ESearchCase::CaseSensitive)) return Title;
        const FString Rest = Title.Mid(Resource.Len()).TrimStart();
        return (Rest.IsEmpty() || Rest.StartsWith(FString::Chr(0x2192))) ? FString() : Title;
    }
    /** Runs of the same stop (a pool lap starts with copies of its depot that only carry the target index) collapse into one chip. */
    inline int32 DTTRouteChips(const TArray<FString>& Stops)
    {
        int32 Chips = 0;
        for (int32 Index = 0; Index < Stops.Num(); ++Index) if (Index == 0 || Stops[Index] != Stops[Index - 1]) ++Chips;
        return Chips;
    }

    /** Card header: an icon and the title on the left, a one-line summary on the right. */
    inline void DTTCardHeader(FDTTCardSink& Sink, UVerticalBox* Body, const FString& Title, const FLinearColor& TitleColor, const FString& Summary, UTexture2D* Icon = nullptr)
    {
        UHorizontalBox* Row = Sink.HBox(Body, FMargin(0, 0, 0, 2));
        Sink.Icon(Row, Icon, 28, FMargin(0, 0, 8, 0));
        Sink.Text(Row, Title, 17, TitleColor, false, FMargin(0), true);
        Sink.Text(Row, Summary, 12, DTTGrey);
    }
}
