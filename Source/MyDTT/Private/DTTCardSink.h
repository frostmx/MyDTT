#pragma once

// The window's drawing kit: the card sink, the palette and the small text helpers every page
// shares. Split out of DynamicTimeTableWidget.cpp, which was the whole window in one file.

#include "CoreMinimal.h"
#include "DynamicTimeTableWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"

class UTexture2D;

DECLARE_LOG_CATEGORY_EXTERN(LogDynamicTimeTableWidget, Log, All);

namespace DTTUI
{
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
    int32 TextIndex = 0, BorderIndex = 0, BarIndex = 0, ImageIndex = 0;

    FDTTCardSink(UWidgetTree* InTree, bool bInBuild, FDTTCardWidgets& InCard) : Tree(InTree), bBuild(bInBuild), Card(InCard) {}

    /** True when an update walked past what the build stored: the shape changed without the key noticing. */
    bool Overran() const { return TextIndex != Card.Texts.Num() || BorderIndex != Card.Borders.Num() || BarIndex != Card.Bars.Num() || ImageIndex != Card.Images.Num(); }

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

    /** The card itself: a tinted border with a vertical body, stored as the card's root. Returns the body (null on an update). */
    UVerticalBox* Root(const FLinearColor& Tint)
    {
        UVerticalBox* Body = nullptr;
        if (bBuild)
        {
            UBorder* Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrushColor(Tint); Border->SetPadding(FMargin(12, 8));
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
    void Icon(UPanelWidget* Parent, UTexture2D* Texture, float Size, const FMargin& Margin = FMargin(0, 0, 6, 0))
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
        // The size box is what takes the space, so hide that.
        UWidget* Holder = Image->GetParent() ? static_cast<UWidget*>(Image->GetParent()) : static_cast<UWidget*>(Image);
        Holder->SetVisibility(Texture ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }

    /** A thin fill bar; Percent 0..1. */
    void Bar(UPanelWidget* Parent, float Percent, const FLinearColor& Color, const FMargin& Margin = FMargin(0, 3, 0, 2))
    {
        Percent = FMath::Clamp(Percent, 0.0f, 1.0f);
        if (bBuild)
        {
            UProgressBar* B = Tree->ConstructWidget<UProgressBar>();
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
};

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
