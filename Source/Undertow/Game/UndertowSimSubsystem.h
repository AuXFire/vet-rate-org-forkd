// Owns the simulation for a game world: loads Content/Data, ticks it with the player's state, and turns its
// events into Blueprint delegates for the HUD and native events for the managers that spawn actors.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Game/UndertowTypes.h"
#include "UndertowSimSubsystem.generated.h"

namespace ut
{
	class Simulation;
	struct Event;
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUndertowHeatChanged, int32, NewLevel, int32, OldLevel);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUndertowCashChanged, int32, Balance, int32, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUndertowObjectiveChanged, FText, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUndertowMissionEnded, bool, bCompleted, FText, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUndertowRoadsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUndertowOriginRequest, double, Latitude, double, Longitude);
DECLARE_MULTICAST_DELEGATE_OneParam(FUndertowSimEvent, const ut::Event&);

UCLASS()
class UNDERTOW_API UUndertowSimSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// Null if the data files failed to load (see the log).
	ut::Simulation* GetSim() const { return Sim; }
	// Every simulation event, in order, for native listeners (traffic and police managers).
	FUndertowSimEvent OnSimEvent;
	// The police manager reports real line-of-sight here each frame. Until something does, the simulation
	// falls back to "a unit is within range".
	void SetPoliceSight(bool bSeen);

	UPROPERTY(BlueprintAssignable, Category = "Undertow")
	FUndertowHeatChanged OnHeatChanged;
	UPROPERTY(BlueprintAssignable, Category = "Undertow")
	FUndertowCashChanged OnCashChanged;
	UPROPERTY(BlueprintAssignable, Category = "Undertow")
	FUndertowObjectiveChanged OnObjectiveChanged;
	UPROPERTY(BlueprintAssignable, Category = "Undertow")
	FUndertowMissionEnded OnMissionEnded;

	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void ReportCrime(EUndertowCrime Crime, FVector Location, bool bSeenByPolice, int32 CivilianWitnesses);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool StartMission(const FString& MissionId);
	// Tell the active mission something happened: "collected", "eliminated", "vehicle_destroyed" or "player_down".
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void NotifyMission(FName Notice, const FString& Tag);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool BuyItem(const FString& ItemId);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool BuyProperty(const FString& PropertyId);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool SaveToSlot(const FString& SlotName);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool LoadFromSlot(const FString& SlotName);

	UFUNCTION(BlueprintPure, Category = "Undertow")
	int32 GetHeatLevel() const;
	// 0..1 while the police are searching: how close the player is to losing them.
	UFUNCTION(BlueprintPure, Category = "Undertow")
	float GetEvadeProgress() const;
	UFUNCTION(BlueprintPure, Category = "Undertow")
	int32 GetCash() const;
	UFUNCTION(BlueprintPure, Category = "Undertow")
	FText GetClockText() const;
	UFUNCTION(BlueprintPure, Category = "Undertow")
	FString GetWeatherName() const;
	UFUNCTION(BlueprintPure, Category = "Undertow")
	FText GetDistrictName() const;
	UFUNCTION(BlueprintPure, Category = "Undertow")
	FText GetObjectiveText() const;
	// Seconds left on the current objective, or -1 when it has no time limit.
	UFUNCTION(BlueprintPure, Category = "Undertow")
	float GetObjectiveTimeRemaining() const;
	// World location of a named place from city.json (safehouses, shops, mission markers). False if unknown.
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool GetLocation(const FString& PoiId, FVector& OutLocation) const;

	// --- Full-scale Earth (Docs/EARTH.md)

	// Says where on the globe this level's origin is, with X pointing east and Y south there: the convention
	// of a CesiumGeoreference. Call it when the level starts and again whenever the georeference origin moves.
	// Without a call, the origin is taken to be the anchor in city.json.
	UFUNCTION(BlueprintCallable, Category = "Undertow|Earth")
	void SetWorldOrigin(double Latitude, double Longitude);
	UFUNCTION(BlueprintPure, Category = "Undertow|Earth")
	void GetWorldOrigin(double& Latitude, double& Longitude) const;
	UFUNCTION(BlueprintPure, Category = "Undertow|Earth")
	void WorldToLatLon(FVector Location, double& Latitude, double& Longitude) const;
	UFUNCTION(BlueprintPure, Category = "Undertow|Earth")
	FVector LatLonToWorld(double Latitude, double Longitude, double ZCm) const;
	// Where no road tile file exists, invent a street grid instead of leaving the area empty. For trying a
	// place before its map data has been converted.
	UFUNCTION(BlueprintCallable, Category = "Undertow|Earth")
	void SetProceduralRoads(bool bEnabled);
	// The road network around the player was rebuilt (new tiles arrived, old ones were dropped).
	UPROPERTY(BlueprintAssignable, Category = "Undertow|Earth")
	FUndertowRoadsChanged OnRoadsChanged;
	// The game needs the level's origin somewhere else on the globe, for example after loading a save made in
	// another city. Move the georeference there and call SetWorldOrigin from the handler.
	UPROPERTY(BlueprintAssignable, Category = "Undertow|Earth")
	FUndertowOriginRequest OnWorldOriginRequested;

private:
	void Dispatch(const ut::Event& Event);
	void UpdateFrames();
	void LoadMissingTiles();
	void FollowPlayer(const FVector& PlayerLocation);

	ut::Simulation* Sim = nullptr;
	bool bHasSightSource = false;
	bool bSeenByPolice = false;
	ut::LocalFrame LevelFrame;   // where the level origin sits on the globe
	bool bProceduralRoads = false;
};
