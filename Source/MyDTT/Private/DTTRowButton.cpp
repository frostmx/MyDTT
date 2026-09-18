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
