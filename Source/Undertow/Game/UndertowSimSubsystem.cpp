#include "Game/UndertowSimSubsystem.h"

#include "Undertow.h"
#include "Game/UndertowSaveGame.h"
#include "Game/UndertowVehicle.h"
#include "Sim/UtSim.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

static_assert(static_cast<int32>(EUndertowCrime::Trespass) == static_cast<int32>(ut::Crime::Trespass), "EUndertowCrime must mirror ut::Crime");
static_assert(static_cast<int32>(EUndertowCrime::Gunfire) == static_cast<int32>(ut::Crime::Gunfire), "EUndertowCrime must mirror ut::Crime");
static_assert(static_cast<int32>(EUndertowCrime::AssaultOnOfficer) + 1 == static_cast<int32>(ut::Crime::Count), "EUndertowCrime must mirror ut::Crime");

namespace UndertowSimPrivate
{
	static bool LoadText(const TCHAR* FileName, std::string& Out)
	{
		const FString Path = FPaths::ProjectContentDir() / TEXT("Data") / FileName;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogUndertow, Error, TEXT("Cannot read %s"), *Path);
			return false;
		}
		Out = UndertowConvert::ToStd(Text);
		return true;
	}
}

UndertowConvert::FFrames& UndertowConvert::Frames()
{
	static FFrames Shared;
	return Shared;
}

bool UUndertowSimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UUndertowSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	std::string City, Tuning, Missions, Economy, Error;
	if (!UndertowSimPrivate::LoadText(TEXT("city.json"), City) || !UndertowSimPrivate::LoadText(TEXT("tuning.json"), Tuning) ||
		!UndertowSimPrivate::LoadText(TEXT("missions.json"), Missions) || !UndertowSimPrivate::LoadText(TEXT("economy.json"), Economy))
	{
		return;
	}
	ut::Simulation* NewSim = new ut::Simulation(static_cast<uint64_t>(FDateTime::Now().GetTicks()));
	if (!NewSim->loadCity(City, &Error) || !NewSim->loadTuning(Tuning, &Error) || !NewSim->loadMissions(Missions, &Error) || !NewSim->loadEconomy(Economy, &Error))
	{
		UE_LOG(LogUndertow, Error, TEXT("Bad game data: %s"), *UndertowConvert::ToFString(Error));
		delete NewSim;
		return;
	}
	Sim = NewSim;
	LevelFrame = Sim->frame();
	UpdateFrames();
	UE_LOG(LogUndertow, Log, TEXT("Simulation ready: %s, %d junctions, %d missions"), *UndertowConvert::ToFString(Sim->city.name()),
		Sim->roads.nodeCount(), static_cast<int32>(Sim->missions.missions().size()));
	if (Sim->worldEnabled())
	{
		UE_LOG(LogUndertow, Log, TEXT("World mode: level origin at %.5f, %.5f until SetWorldOrigin says otherwise"), LevelFrame.anchor().lat, LevelFrame.anchor().lon);
	}
}

void UUndertowSimSubsystem::Deinitialize()
{
	delete Sim;
	Sim = nullptr;
	UpdateFrames();
	Super::Deinitialize();
}

TStatId UUndertowSimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UUndertowSimSubsystem, STATGROUP_Tickables);
}

void UUndertowSimSubsystem::SetPoliceSight(bool bSeen)
{
	bHasSightSource = true;
	bSeenByPolice = bSeen;
}

void UUndertowSimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Sim)
	{
		return;
	}
	ut::PlayerInput Input;
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (const APawn* Pawn = PC ? PC->GetPawn() : nullptr)
	{
		FollowPlayer(Pawn->GetActorLocation());
		Input.pos = UndertowConvert::ToSim(Pawn->GetActorLocation());
		Input.velocity = UndertowConvert::VelocityToSim(Pawn->GetVelocity());
		if (const AUndertowVehicle* Vehicle = Cast<AUndertowVehicle>(Pawn))
		{
			Input.inVehicle = true;
			Input.vehicleTag = UndertowConvert::ToStd(Vehicle->VehicleTag.ToString());
		}
	}
	Input.hasSightResult = bHasSightSource;
	Input.seenByPolice = bSeenByPolice;

	if (Sim->worldEnabled())
	{
		LoadMissingTiles();
	}
	Sim->tick(DeltaTime, Input);
	for (const ut::Event& Event : Sim->drainEvents())
	{
		Dispatch(Event);
	}
}

void UUndertowSimSubsystem::Dispatch(const ut::Event& Event)
{
	OnSimEvent.Broadcast(Event);
	switch (Event.type)
	{
	case ut::EventType::HeatLevelChanged:
		OnHeatChanged.Broadcast(Event.a, Event.b);
		break;
	case ut::EventType::CashChanged:
		OnCashChanged.Broadcast(Event.a, Event.b);
		break;
	case ut::EventType::ObjectiveChanged:
		OnObjectiveChanged.Broadcast(FText::FromString(UndertowConvert::ToFString(Event.text)));
		break;
	case ut::EventType::MissionCompleted:
		OnMissionEnded.Broadcast(true, FText::Format(NSLOCTEXT("Undertow", "MissionPassed", "Mission passed  +${0}"), FText::AsNumber(Event.a)));
		break;
	case ut::EventType::MissionFailed:
		OnMissionEnded.Broadcast(false, FText::FromString(UndertowConvert::ToFString(Event.text)));
		break;
	case ut::EventType::RoadsChanged:
		OnRoadsChanged.Broadcast();
		break;
	default:
		break;
	}
}

void UUndertowSimSubsystem::ReportCrime(EUndertowCrime Crime, FVector Location, bool bInSeenByPolice, int32 CivilianWitnesses)
{
	if (Sim)
	{
		Sim->reportCrime(static_cast<ut::Crime>(static_cast<uint8>(Crime)), UndertowConvert::ToSim(Location), bInSeenByPolice, CivilianWitnesses);
	}
}

bool UUndertowSimSubsystem::StartMission(const FString& MissionId)
{
	return Sim && Sim->startMission(UndertowConvert::ToStd(MissionId));
}

void UUndertowSimSubsystem::NotifyMission(FName Notice, const FString& Tag)
{
	if (!Sim)
	{
		return;
	}
	ut::MissionNotice Kind;
	if (Notice == FName(TEXT("collected"))) { Kind = ut::MissionNotice::Collected; }
	else if (Notice == FName(TEXT("eliminated"))) { Kind = ut::MissionNotice::Eliminated; }
	else if (Notice == FName(TEXT("vehicle_destroyed"))) { Kind = ut::MissionNotice::VehicleDestroyed; }
	else if (Notice == FName(TEXT("player_down"))) { Kind = ut::MissionNotice::PlayerDown; }
	else
	{
		UE_LOG(LogUndertow, Warning, TEXT("NotifyMission: unknown notice '%s'"), *Notice.ToString());
		return;
	}
	Sim->notifyMission(Kind, UndertowConvert::ToStd(Tag));
}

bool UUndertowSimSubsystem::BuyItem(const FString& ItemId) { return Sim && Sim->buyItem(UndertowConvert::ToStd(ItemId)); }
bool UUndertowSimSubsystem::BuyProperty(const FString& PropertyId) { return Sim && Sim->buyProperty(UndertowConvert::ToStd(PropertyId)); }

bool UUndertowSimSubsystem::SaveToSlot(const FString& SlotName)
{
	if (!Sim)
	{
		return false;
	}
	UUndertowSaveGame* Save = Cast<UUndertowSaveGame>(UGameplayStatics::CreateSaveGameObject(UUndertowSaveGame::StaticClass()));
	if (!Save)
	{
		return false;
	}
	Save->SimulationJson = UndertowConvert::ToFString(Sim->save());
	return UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
}

bool UUndertowSimSubsystem::LoadFromSlot(const FString& SlotName)
{
	const UUndertowSaveGame* Save = Cast<UUndertowSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (!Sim || !Save)
	{
		return false;
	}
	std::string Error;
	if (!Sim->load(UndertowConvert::ToStd(Save->SimulationJson), &Error))
	{
		UE_LOG(LogUndertow, Warning, TEXT("Save '%s' was not loaded: %s"), *SlotName, *UndertowConvert::ToFString(Error));
		return false;
	}
	if (Sim->worldEnabled())
	{
		// The save may have been made on the other side of the planet. Ask the level to move its origin there
		// first (the handler calls SetWorldOrigin), then line the two frames up again.
		const ut::LatLon Where = Sim->playerGeo();
		if (ut::geo::distance(LevelFrame.anchor(), Where) > 10000.0)
		{
			OnWorldOriginRequested.Broadcast(Where.lat, Where.lon);
		}
		UpdateFrames();
	}
	// Put the player back where they saved. Traffic and police actors rebuild themselves from simulation events.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			FVector Location = UndertowConvert::ToWorld(Sim->savedPlayerPosition(), Pawn->GetActorLocation().Z + 50.0);
			// Stand on whatever ground is loaded there. Streamed terrain may not have arrived yet; see Docs/EARTH.md.
			FHitResult Hit;
			if (GetWorld()->LineTraceSingleByObjectType(Hit, Location + FVector(0.0, 0.0, 500000.0), Location - FVector(0.0, 0.0, 500000.0), FCollisionObjectQueryParams(ECC_WorldStatic)))
			{
				Location.Z = Hit.ImpactPoint.Z + 120.0;
			}
			Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	return true;
}

void UUndertowSimSubsystem::UpdateFrames()
{
	UndertowConvert::FFrames& Frames = UndertowConvert::Frames();
	Frames = UndertowConvert::FFrames();
	if (Sim && Sim->worldEnabled())
	{
		// Exact where the simulation's anchor is, which is where the player is.
		Frames.SimToLevel = ut::FrameMap(Sim->frame(), LevelFrame, Sim->frame().anchor());
		Frames.LevelToSim = Frames.SimToLevel.inverse();
	}
}

void UUndertowSimSubsystem::FollowPlayer(const FVector& PlayerLocation)
{
	if (!Sim->worldEnabled())
	{
		return;
	}
	// Keep the simulation's anchor near the player, wherever the level's own origin is.
	const ut::LatLon Where = LevelFrame.toGeo(PlayerLocation.X * 0.01, -PlayerLocation.Y * 0.01);
	if (ut::geo::distance(Where, Sim->frame().anchor()) > static_cast<double>(Sim->rebaseDistance))
	{
		Sim->setAnchor(Where);
		UpdateFrames();
	}
}

void UUndertowSimSubsystem::LoadMissingTiles()
{
	// A few per frame, nearest first, so a burst of new tiles does not stall the frame.
	const std::vector<ut::TileKey> Missing = Sim->world.missing();   // a copy: providing a tile edits the list
	int32 Budget = 3;
	for (const ut::TileKey& Key : Missing)
	{
		if (Budget-- <= 0)
		{
			break;
		}
		const FString Path = FPaths::ProjectContentDir() / FString::Printf(TEXT("Data/World/tiles/%d/%d/%d.json"), Key.z, Key.x, Key.y);
		FString Text;
		if (FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Text, *Path))
		{
			std::string Error;
			if (!Sim->provideTile(Key, UndertowConvert::ToStd(Text), &Error))
			{
				UE_LOG(LogUndertow, Warning, TEXT("Road tile %s ignored: %s"), *Path, *UndertowConvert::ToFString(Error));
			}
		}
		else if (bProceduralRoads)
		{
			Sim->world.provide(Key, ut::WorldRoads::proceduralTile(Key));
		}
		else
		{
			Sim->provideTile(Key, std::string());   // no data here: stop asking
		}
	}
}

void UUndertowSimSubsystem::SetWorldOrigin(double Latitude, double Longitude)
{
	if (!Sim)
	{
		return;
	}
	LevelFrame = ut::LocalFrame(ut::LatLon(Latitude, Longitude));
	if (!Sim->worldEnabled())
	{
		Sim->enableWorld(LevelFrame.anchor());
	}
	UpdateFrames();   // the simulation's own anchor catches up with the player on the next tick
}

void UUndertowSimSubsystem::GetWorldOrigin(double& Latitude, double& Longitude) const
{
	Latitude = LevelFrame.anchor().lat;
	Longitude = LevelFrame.anchor().lon;
}

void UUndertowSimSubsystem::WorldToLatLon(FVector Location, double& Latitude, double& Longitude) const
{
	const ut::LatLon Where = LevelFrame.toGeo(Location.X * 0.01, -Location.Y * 0.01);
	Latitude = Where.lat;
	Longitude = Where.lon;
}

FVector UUndertowSimSubsystem::LatLonToWorld(double Latitude, double Longitude, double ZCm) const
{
	double East = 0.0, North = 0.0;
	LevelFrame.toLocal(ut::LatLon(Latitude, Longitude), East, North);
	return FVector(East * 100.0, -North * 100.0, ZCm);
}

void UUndertowSimSubsystem::SetProceduralRoads(bool bEnabled)
{
	bProceduralRoads = bEnabled;
}

int32 UUndertowSimSubsystem::GetHeatLevel() const { return Sim ? Sim->heat.level() : 0; }
float UUndertowSimSubsystem::GetEvadeProgress() const { return Sim ? Sim->heat.evadeProgress() : 0.0f; }
int32 UUndertowSimSubsystem::GetCash() const { return Sim ? Sim->economy.cash() : 0; }

FText UUndertowSimSubsystem::GetClockText() const
{
	if (!Sim)
	{
		return FText::GetEmpty();
	}
	const int32 Minutes = static_cast<int32>(Sim->clock.secondsOfDay / 60.0f);
	return FText::FromString(FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60));
}

FString UUndertowSimSubsystem::GetWeatherName() const
{
	return Sim ? FString(UTF8_TO_TCHAR(ut::weatherName(Sim->weather.current()))) : FString();
}

FText UUndertowSimSubsystem::GetDistrictName() const
{
	const ut::District* District = Sim ? Sim->currentDistrict() : nullptr;
	return District ? FText::FromString(UndertowConvert::ToFString(District->name)) : FText::GetEmpty();
}

FText UUndertowSimSubsystem::GetObjectiveText() const
{
	const ut::Objective* Objective = Sim ? Sim->missions.currentObjective() : nullptr;
	return Objective ? FText::FromString(UndertowConvert::ToFString(Objective->text)) : FText::GetEmpty();
}

float UUndertowSimSubsystem::GetObjectiveTimeRemaining() const { return Sim ? Sim->missions.timeRemaining() : -1.0f; }

bool UUndertowSimSubsystem::GetLocation(const FString& PoiId, FVector& OutLocation) const
{
	const ut::Poi* Poi = Sim ? Sim->city.findPoi(UndertowConvert::ToStd(PoiId)) : nullptr;
	if (!Poi)
	{
		return false;
	}
	OutLocation = UndertowConvert::ToWorld(Poi->pos);
	return true;
}
