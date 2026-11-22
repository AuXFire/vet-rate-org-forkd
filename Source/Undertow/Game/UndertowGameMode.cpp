#include "Game/UndertowGameMode.h"

#include "Game/UndertowCharacter.h"
#include "Game/UndertowSimSubsystem.h"
#include "Game/UndertowTrafficManager.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

AUndertowGameMode::AUndertowGameMode()
{
	DefaultPawnClass = AUndertowCharacter::StaticClass();
	TrafficManagerClass = AUndertowTrafficManager::StaticClass();
}

void AUndertowGameMode::StartPlay()
{
	Super::StartPlay();
	// One manager per level: use the one placed by the designer if there is one.
	if (TrafficManagerClass && !TActorIterator<AUndertowTrafficManager>(GetWorld()))
	{
		GetWorld()->SpawnActor<AUndertowTrafficManager>(TrafficManagerClass, FTransform::Identity);
	}
}

void AUndertowGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
	APawn* Pawn = NewPlayer ? NewPlayer->GetPawn() : nullptr;
	const UUndertowSimSubsystem* Sim = GetWorld()->GetSubsystem<UUndertowSimSubsystem>();
	FVector Start;
	if (Pawn && Sim && Sim->GetLocation(StartLocationId, Start))
	{
		Start.Z = Pawn->GetActorLocation().Z + 150.0;
		Pawn->SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
	}
}
