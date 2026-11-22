#include "Game/UndertowHUDWidget.h"

#include "Game/UndertowSimSubsystem.h"

#include "Engine/World.h"

UUndertowSimSubsystem* UUndertowHUDWidget::GetSimulation() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UUndertowSimSubsystem>() : nullptr;
}

void UUndertowHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UUndertowSimSubsystem* Sim = GetSimulation())
	{
		Sim->OnHeatChanged.AddDynamic(this, &UUndertowHUDWidget::HandleHeat);
		Sim->OnCashChanged.AddDynamic(this, &UUndertowHUDWidget::HandleCash);
		Sim->OnObjectiveChanged.AddDynamic(this, &UUndertowHUDWidget::HandleObjective);
		Sim->OnMissionEnded.AddDynamic(this, &UUndertowHUDWidget::HandleMissionEnded);
		// Show the current state straight away instead of waiting for the first change.
		OnHeatLevelChanged(Sim->GetHeatLevel(), 0);
		OnCashChanged(Sim->GetCash(), 0);
		OnObjectiveChanged(Sim->GetObjectiveText());
	}
}

void UUndertowHUDWidget::NativeDestruct()
{
	if (UUndertowSimSubsystem* Sim = GetSimulation())
	{
		Sim->OnHeatChanged.RemoveDynamic(this, &UUndertowHUDWidget::HandleHeat);
		Sim->OnCashChanged.RemoveDynamic(this, &UUndertowHUDWidget::HandleCash);
		Sim->OnObjectiveChanged.RemoveDynamic(this, &UUndertowHUDWidget::HandleObjective);
		Sim->OnMissionEnded.RemoveDynamic(this, &UUndertowHUDWidget::HandleMissionEnded);
	}
	Super::NativeDestruct();
}

void UUndertowHUDWidget::HandleHeat(int32 NewLevel, int32 OldLevel) { OnHeatLevelChanged(NewLevel, OldLevel); }
void UUndertowHUDWidget::HandleCash(int32 Balance, int32 Delta) { OnCashChanged(Balance, Delta); }
void UUndertowHUDWidget::HandleObjective(FText Text) { OnObjectiveChanged(Text); }
void UUndertowHUDWidget::HandleMissionEnded(bool bCompleted, FText Message) { OnMissionEnded(bCompleted, Message); }
