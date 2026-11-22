// Level actors that connect the world to the mission system: a marker that starts a mission when the player
// walks into it, and a pickup that counts toward a "collect" objective.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UndertowMissionActors.generated.h"

class USphereComponent;

UCLASS()
class UNDERTOW_API AUndertowMissionMarker : public AActor
{
	GENERATED_BODY()

public:
	AUndertowMissionMarker();

	// A mission id from Content/Data/missions.json, e.g. "m01_slack_water".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	FString MissionId;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Undertow")
	TObjectPtr<USphereComponent> Trigger;

	// Fired when the mission actually starts (it will not if it is locked or another one is running).
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnMissionStarted();

private:
	UFUNCTION()
	void HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};

UCLASS()
class UNDERTOW_API AUndertowPickup : public AActor
{
	GENERATED_BODY()

public:
	AUndertowPickup();

	// The tag a "collect" objective is waiting for, e.g. "ledger".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	FString Tag;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Undertow")
	TObjectPtr<USphereComponent> Trigger;

private:
	UFUNCTION()
	void HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};
