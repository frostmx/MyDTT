#pragma once

// The window's drawing kit: the card sink, the palette and the small text helpers every page
// shares. Split out of DynamicTimeTableWidget.cpp, which was the whole window in one file.

#include "CoreMinimal.h"
#include "DynamicTimeTableWidget.h"
#include "DTTRowButton.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
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
    inline const FLinearColor DTTCardTint(0.08f, 0.12f, 0.15f, 1.0f);
    inline const FLinearColor DTTCardTintOpen(0.09f, 0.16f, 0.13f, 1.0f);
    inline const FLinearColor DTTCardTintWait(0.16f, 0.14f, 0.07f, 1.0f);
    inline const FLinearColor DTTCardTintWarn(0.17f, 0.11f, 0.08f, 1.0f);
    inline const FLinearColor DTTCyan(0.2f, 0.8f, 1.0f);
    inline const FLinearColor DTTGreen(0.3f, 1.0f, 0.5f);
    inline const FLinearColor DTTYellow(1.0f, 0.85f, 0.3f);
    inline const FLinearColor DTTOrange(1.0f, 0.55f, 0.1f);
    inline const FLinearColor DTTRed(1.0f, 0.35f, 0.3f);
    inline const FLinearColor DTTGrey(0.6f, 0.68f, 0.74f);
    inline const FLinearColor DTTDim(0.45f, 0.5f, 0.55f);
    inline const FLinearColor DTTSteel(0.6f, 0.7f, 0.85f);
    /** A requester that is stocked and quiet. Steel grey read as an empty bar against the
        progress bar's own light track, which is the one thing a stock bar must never do. */
    inline const FLinearColor DTTStocked(0.35f, 0.66f, 0.92f);

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

    inline UButton* MakeButton(UWidgetTree* Tree, const FString& Label)
    {
        UButton* Button = Tree->ConstructWidget<UButton>();
        Button->SetContent(MakeText(Tree, Label, 15, FLinearColor::White));
        return Button;
    }

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
    inline constexpr float DTTRowHeight = 46.0f;
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
    inline constexpr float DTTColMode = 64.0f;
    inline constexpr float DTTColStock = 62.0f;
    inline constexpr float DTTColLimit = 48.0f;
    /** One station's own bar, wider than the eight that share a resource row. */
    inline constexpr float DTTStockBarWide = 96.0f;
    /** One requester's bar inside a resource row; eight of them fit before the row gives up. */
    inline constexpr float DTTStockBarWidth = 54.0f;
    inline constexpr int32 DTTMaxStockBars = 8;

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

    /** A column of a Row: fixed width so the columns line up, or Width 0 to take what is left. */
    UTextBlock* Cell(UPanelWidget* Line, const FString& Value, int32 Size, const FLinearColor& Color,
        float Width, EHorizontalAlignment Align = HAlign_Left, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        UTextBlock* T = nullptr;
        if (bBuild)
        {
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            if (Width > 0.0f) Box->SetWidthOverride(Width);
            T = Text(nullptr, Value, Size, Color);
            // A size box holds a column's width but does not stop the text inside it from drawing
            // past that: a long station name printed straight over the next column. The ellipsis
            // says the name goes on, which a clean cut would not.
            T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
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
                Pad<UVerticalBoxSlot>(Box, Margin);
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
