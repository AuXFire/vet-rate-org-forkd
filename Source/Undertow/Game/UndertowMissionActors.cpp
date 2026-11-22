#include "Game/UndertowMissionActors.h"

#include "Game/UndertowSimSubsystem.h"

#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

namespace UndertowMissionPrivate
{
	static USphereComponent* MakeTrigger(AActor* Owner, float Radius)
	{
		USphereComponent* Sphere = Owner->CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
		Sphere->InitSphereRadius(Radius);
		Sphere->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
		Owner->SetRootComponent(Sphere);
		return Sphere;
	}

	static bool IsPlayer(const AActor* Actor)
	{
		const APawn* Pawn = Cast<APawn>(Actor);
		return Pawn && Pawn->IsPlayerControlled();
	}
}

AUndertowMissionMarker::AUndertowMissionMarker()
{
	Trigger = UndertowMissionPrivate::MakeTrigger(this, 250.0f);
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AUndertowMissionMarker::HandleOverlap);
}

void AUndertowMissionMarker::HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	UUndertowSimSubsystem* Sim = GetWorld()->GetSubsystem<UUndertowSimSubsystem>();
	if (Sim && UndertowMissionPrivate::IsPlayer(OtherActor) && Sim->StartMission(MissionId))
	{
		OnMissionStarted();
	}
}

AUndertowPickup::AUndertowPickup()
{
	Trigger = UndertowMissionPrivate::MakeTrigger(this, 120.0f);
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AUndertowPickup::HandleOverlap);
}

void AUndertowPickup::HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	UUndertowSimSubsystem* Sim = GetWorld()->GetSubsystem<UUndertowSimSubsystem>();
	if (Sim && UndertowMissionPrivate::IsPlayer(OtherActor))
	{
		Sim->NotifyMission(TEXT("collected"), Tag);
		Destroy();
	}
}
