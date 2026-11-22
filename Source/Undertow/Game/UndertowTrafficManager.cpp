#include "Game/UndertowTrafficManager.h"

#include "Undertow.h"
#include "Game/UndertowAIDriverComponent.h"
#include "Game/UndertowSimSubsystem.h"
#include "Game/UndertowVehicle.h"
#include "Sim/UtSim.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

AUndertowTrafficManager::AUndertowTrafficManager()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the simulation subsystem has ticked this frame, so actors show this frame's positions.
	PrimaryActorTick.TickGroup = TG_PostPhysics;
}

void AUndertowTrafficManager::BeginPlay()
{
	Super::BeginPlay();
	Subsystem = GetWorld()->GetSubsystem<UUndertowSimSubsystem>();
	if (Subsystem)
	{
		EventHandle = Subsystem->OnSimEvent.AddUObject(this, &AUndertowTrafficManager::HandleSimEvent);
	}
	if (AmbientVehicleClasses.Num() == 0)
	{
		UE_LOG(LogUndertow, Warning, TEXT("Traffic manager has no AmbientVehicleClasses: ambient traffic is simulated but invisible."));
	}
}

void AUndertowTrafficManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Subsystem)
	{
		Subsystem->OnSimEvent.Remove(EventHandle);
	}
	Super::EndPlay(EndPlayReason);
}

double AUndertowTrafficManager::GroundZ(const FVector& Location, bool bFromAbove) const
{
	double Reference = Location.Z, Up = 500.0, Down = 2000.0;
	if (bFromAbove)
	{
		// Real terrain is not at height zero: start from the player's height and look 300 m either way.
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		Reference = Pawn ? Pawn->GetActorLocation().Z : Location.Z;
		Up = 30000.0;
		Down = 30000.0;
	}
	FHitResult Hit;
	const FVector Start(Location.X, Location.Y, Reference + Up);
	const FVector End(Location.X, Location.Y, Reference - Down);
	const bool bHit = GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic));
	return bHit ? Hit.ImpactPoint.Z : Reference;
}

AActor* AUndertowTrafficManager::AcquireAmbient(int32 Archetype, const FVector& Location, const FRotator& Rotation)
{
	if (AmbientVehicleClasses.Num() == 0)
	{
		return nullptr;
	}
	UClass* Wanted = AmbientVehicleClasses[Archetype % AmbientVehicleClasses.Num()];
	if (!Wanted)
	{
		return nullptr;
	}
	for (int32 i = AmbientPool.Num() - 1; i >= 0; --i)
	{
		AActor* Pooled = AmbientPool[i];
		if (Pooled && Pooled->GetClass() == Wanted)
		{
			AmbientPool.RemoveAtSwap(i);
			Pooled->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
			Pooled->SetActorHiddenInGame(false);
			Pooled->SetActorEnableCollision(true);
			return Pooled;
		}
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return GetWorld()->SpawnActor<AActor>(Wanted, Location, Rotation, Params);
}

void AUndertowTrafficManager::ReleaseAmbient(int32 AgentId)
{
	TObjectPtr<AActor> Actor;
	if (AmbientByAgent.RemoveAndCopyValue(AgentId, Actor) && Actor)
	{
		Actor->SetActorHiddenInGame(true);
		Actor->SetActorEnableCollision(false);
		AmbientPool.Add(Actor);
	}
}

void AUndertowTrafficManager::HandleSimEvent(const ut::Event& Event)
{
	switch (Event.type)
	{
	case ut::EventType::TrafficSpawned:
	{
		FVector Location = UndertowConvert::ToWorld(Event.pos);
		Location.Z = GroundZ(Location, true);
		if (AActor* Actor = AcquireAmbient(Event.b, Location, FRotator::ZeroRotator))
		{
			AmbientByAgent.Add(Event.a, Actor);
		}
		break;
	}
	case ut::EventType::TrafficRemoved:
		ReleaseAmbient(Event.a);
		break;
	case ut::EventType::UnitSpawned:
	{
		if (!PoliceVehicleClass)
		{
			break;   // dispatch still runs; without a class the units stay simulation-only
		}
		FVector Location = UndertowConvert::ToWorld(Event.pos);
		Location.Z = GroundZ(Location, true) + 80.0;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		if (AUndertowVehicle* Vehicle = GetWorld()->SpawnActor<AUndertowVehicle>(PoliceVehicleClass, Location, FRotator::ZeroRotator, Params))
		{
			Vehicle->bOwnedByPlayer = false;
			Vehicle->VehicleTag = TEXT("police");
			Vehicle->SpawnDefaultController();   // Chaos vehicles only accept inputs while something controls them
			if (!Vehicle->FindComponentByClass<UUndertowAIDriverComponent>())
			{
				UUndertowAIDriverComponent* Driver = NewObject<UUndertowAIDriverComponent>(Vehicle);
				Driver->RegisterComponent();
			}
			PoliceByUnit.Add(Event.a, Vehicle);
			PolicePathVersion.Add(Event.a, -1);
		}
		break;
	}
	case ut::EventType::UnitRemoved:
	{
		TObjectPtr<AUndertowVehicle> Vehicle;
		if (PoliceByUnit.RemoveAndCopyValue(Event.a, Vehicle) && Vehicle)
		{
			Vehicle->Destroy();
		}
		PolicePathVersion.Remove(Event.a);
		break;
	}
	default:
		break;
	}
}

void AUndertowTrafficManager::UpdateAmbient(float DeltaTime)
{
	const ut::Simulation* Sim = Subsystem->GetSim();
	for (const ut::TrafficAgent& Agent : Sim->traffic.agents())
	{
		const TObjectPtr<AActor>* Found = AmbientByAgent.Find(Agent.id);
		AActor* Actor = Found ? Found->Get() : nullptr;
		if (!Actor)
		{
			continue;
		}
		FVector Target = UndertowConvert::ToWorld(Agent.pos);
		Target.Z = GroundZ(FVector(Target.X, Target.Y, Actor->GetActorLocation().Z));
		const FRotator TargetRotation(0.0, UndertowConvert::HeadingToYaw(Agent.heading), 0.0);
		// The simulation moves cars lane to lane with no turning arc; easing position and heading hides the
		// corner at junctions.
		const FVector Location = FMath::VInterpTo(Actor->GetActorLocation(), Target, DeltaTime, 12.0f);
		const FRotator Rotation = FMath::RInterpTo(Actor->GetActorRotation(), TargetRotation, DeltaTime, TurnInterpSpeed);
		Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AUndertowTrafficManager::UpdatePolice()
{
	ut::Simulation* Sim = Subsystem->GetSim();
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Player = PC ? PC->GetPawn() : nullptr;
	const double SightCm = static_cast<double>(Sim->dispatch.tuning.sightRange * Sim->weather.visibility()) * 100.0;
	bool bSeen = false;

	for (const ut::PoliceUnit& Unit : Sim->dispatch.units())
	{
		const TObjectPtr<AUndertowVehicle>* Found = PoliceByUnit.Find(Unit.id);
		AUndertowVehicle* Vehicle = Found ? Found->Get() : nullptr;
		if (!Vehicle)
		{
			continue;
		}
		if (UUndertowAIDriverComponent* Driver = Vehicle->FindComponentByClass<UUndertowAIDriverComponent>())
		{
			int32& Version = PolicePathVersion.FindOrAdd(Unit.id, -1);
			if (Version != Unit.pathVersion)
			{
				Version = Unit.pathVersion;
				Driver->SetPathSim(Unit.path);
			}
			Driver->CruiseSpeed = Unit.role == ut::UnitRole::Pursue ? Sim->dispatch.tuning.pursuitSpeed
				: Unit.role == ut::UnitRole::Search ? Sim->dispatch.tuning.searchSpeed : 8.0f;
		}
		const FVector Location = Vehicle->GetActorLocation();
		Sim->dispatch.reportUnit(Unit.id, UndertowConvert::ToSim(Location),
			UndertowConvert::YawToHeading(static_cast<float>(Vehicle->GetActorRotation().Yaw)), Vehicle->GetSpeed());

		if (Player && !bSeen && Unit.role != ut::UnitRole::StandDown && FVector::DistSquared(Location, Player->GetActorLocation()) <= FMath::Square(SightCm))
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(UndertowPoliceSight), false, Vehicle);
			Params.AddIgnoredActor(Player);
			FHitResult Hit;
			const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, Location + FVector(0.0, 0.0, 120.0),
				Player->GetActorLocation() + FVector(0.0, 0.0, 60.0), ECC_Visibility, Params);
			bSeen = !bBlocked;
		}
	}
	// Only take over the sight decision when police exist as real vehicles; otherwise the simulation keeps
	// using its own distance check and wanted levels still work.
	if (PoliceVehicleClass)
	{
		Subsystem->SetPoliceSight(bSeen);
	}
}

void AUndertowTrafficManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Subsystem || !Subsystem->GetSim())
	{
		return;
	}
	UpdateAmbient(DeltaTime);
	UpdatePolice();
}
