// Puts the simulation's vehicles into the world.
//
// Ambient traffic: one lightweight actor per simulated car, moved along its lane every frame (no physics),
// recycled through a pool as cars enter and leave the ring around the player.
// Police: real physics vehicles steered by UUndertowAIDriverComponent along the route dispatch gives them.
// This manager also does the line-of-sight traces that decide whether the police can see the player.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UndertowTrafficManager.generated.h"

class AUndertowVehicle;
class UUndertowSimSubsystem;
namespace ut { struct Event; }

UCLASS()
class UNDERTOW_API AUndertowTrafficManager : public AActor
{
	GENERATED_BODY()

public:
	AUndertowTrafficManager();

	virtual void Tick(float DeltaTime) override;

	// One class per traffic archetype, in the order of Content/Data/vehicles.json. Any actor with a mesh
	// works; they are moved kinematically. Leave empty to run without visible ambient traffic.
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	TArray<TSubclassOf<AActor>> AmbientVehicleClasses;
	// A drivable vehicle Blueprint used for police units.
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	TSubclassOf<AUndertowVehicle> PoliceVehicleClass;
	// How quickly ambient cars turn to a new heading at junctions (higher = snappier).
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float TurnInterpSpeed = 6.0f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void HandleSimEvent(const ut::Event& Event);
	AActor* AcquireAmbient(int32 Archetype, const FVector& Location, const FRotator& Rotation);
	void ReleaseAmbient(int32 AgentId);
	// Ground height under a point. `bFromAbove`: search from high above the player (for a vehicle that has just
	// appeared); otherwise search close to Location.Z, which keeps a vehicle on its own level under bridges.
	double GroundZ(const FVector& Location, bool bFromAbove = false) const;
	void UpdateAmbient(float DeltaTime);
	void UpdatePolice();

	UPROPERTY()
	TObjectPtr<UUndertowSimSubsystem> Subsystem;
	UPROPERTY()
	TMap<int32, TObjectPtr<AActor>> AmbientByAgent;
	UPROPERTY()
	TArray<TObjectPtr<AActor>> AmbientPool;
	UPROPERTY()
	TMap<int32, TObjectPtr<AUndertowVehicle>> PoliceByUnit;
	TMap<int32, int32> PolicePathVersion;
	FDelegateHandle EventHandle;
};
