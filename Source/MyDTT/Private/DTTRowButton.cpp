#include "DTTRowButton.h"

void UDTTRowButton::Bind(const FDTTRowActionDelegate& Delegate)
{
    OnAction = Delegate;
    // OnReleased and not OnClicked would match the game's own row buttons, but the dev bridge
    // broadcasts OnClicked, and this is the only way these get pressed without a mouse.
    if (!OnClicked.IsAlreadyBound(this, &UDTTRowButton::HandleClicked))
    {
        OnClicked.AddDynamic(this, &UDTTRowButton::HandleClicked);
    }
}

void UDTTRowButton::HandleClicked()
{
    if (Target.Action == EDTTRowAction::None) return;
    OnAction.ExecuteIfBound(Target);
}

void UDTTRowCheck::Bind(const FDTTRowActionDelegate& Delegate)
{
    OnAction = Delegate;
    if (!OnCheckStateChanged.IsAlreadyBound(this, &UDTTRowCheck::HandleChanged))
    {
        OnCheckStateChanged.AddDynamic(this, &UDTTRowCheck::HandleChanged);
    }
}

void UDTTRowCheck::HandleChanged(bool)
{
    // The state is not passed on: the window holds the selection, and the tick only says which
    // row was touched. Reading it back from the box would make the box the truth, and the box is
    // recycled into another row every refresh.
    if (Target.Action == EDTTRowAction::None) return;
    OnAction.ExecuteIfBound(Target);
}
