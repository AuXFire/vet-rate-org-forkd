// Drives the vehicle it is attached to along a list of world points, using the simulation's AI driver
// (pure pursuit steering, slows for corners, stops at the end). Used by police and scripted mission cars.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sim/UtDriver.h"
#include "UndertowAIDriverComponent.generated.h"

UCLASS(ClassGroup = (Undertow), meta = (BlueprintSpawnableComponent))
class UNDERTOW_API UUndertowAIDriverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUndertowAIDriverComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void SetPath(const TArray<FVector>& WorldPoints);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void ClearPath();
	UFUNCTION(BlueprintPure, Category = "Undertow")
	bool HasArrived() const { return Driver.arrived(); }

	// Speed to hold on straights, meters per second.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	float CruiseSpeed = 14.0f;

	void SetPathSim(const std::vector<ut::Vec2>& Points) { Driver.setPath(Points); }

private:
	ut::Driver Driver;
};
