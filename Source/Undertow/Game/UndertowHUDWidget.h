// Base class for the HUD. Make a Widget Blueprint that inherits from this, lay out the wanted stars, cash,
// clock and objective text, and implement the events below; the C++ side keeps them fed.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UndertowHUDWidget.generated.h"

class UUndertowSimSubsystem;

UCLASS(Abstract)
class UNDERTOW_API UUndertowHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnHeatLevelChanged(int32 NewLevel, int32 OldLevel);
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnCashChanged(int32 Balance, int32 Delta);
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnObjectiveChanged(const FText& Text);
	// Mission passed / failed banner.
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnMissionEnded(bool bCompleted, const FText& Message);

	// For bindings that poll every frame (clock, district name, evade bar).
	UFUNCTION(BlueprintPure, Category = "Undertow")
	UUndertowSimSubsystem* GetSimulation() const;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	UFUNCTION()
	void HandleHeat(int32 NewLevel, int32 OldLevel);
	UFUNCTION()
	void HandleCash(int32 Balance, int32 Delta);
	UFUNCTION()
	void HandleObjective(FText Text);
	UFUNCTION()
	void HandleMissionEnded(bool bCompleted, FText Message);
};
