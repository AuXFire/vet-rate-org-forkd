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
	UE_LOG(LogUndertow, Log, TEXT("Simulation ready: %s, %d junctions, %d missions"), *UndertowConvert::ToFString(Sim->city.name()),
		Sim->roads.nodeCount(), static_cast<int32>(Sim->missions.missions().size()));
}

void UUndertowSimSubsystem::Deinitialize()
{
	delete Sim;
	Sim = nullptr;
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
		Input.pos = UndertowConvert::ToSim(Pawn->GetActorLocation());
		const FVector Velocity = Pawn->GetVelocity();
		Input.velocity = ut::Vec2(static_cast<float>(Velocity.Y * 0.01), static_cast<float>(Velocity.X * 0.01));
		if (const AUndertowVehicle* Vehicle = Cast<AUndertowVehicle>(Pawn))
		{
			Input.inVehicle = true;
			Input.vehicleTag = UndertowConvert::ToStd(Vehicle->VehicleTag.ToString());
		}
	}
	Input.hasSightResult = bHasSightSource;
	Input.seenByPolice = bSeenByPolice;

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
	// Put the player back where they saved. Traffic and police actors rebuild themselves from simulation events.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			Pawn->SetActorLocation(UndertowConvert::ToWorld(Sim->savedPlayerPosition(), Pawn->GetActorLocation().Z + 50.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	return true;
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
