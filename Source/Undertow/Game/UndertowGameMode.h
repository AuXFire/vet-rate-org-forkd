#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "UndertowGameMode.generated.h"

class AUndertowTrafficManager;

// Spawns the player at the first safehouse and makes sure the level has a traffic manager.
UCLASS()
class UNDERTOW_API AUndertowGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AUndertowGameMode();

	virtual void StartPlay() override;
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

	// Location id from city.json where a new game starts.
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	FString StartLocationId = TEXT("safehouse_narrows");
	// Set to a Blueprint subclass that has vehicle classes assigned.
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	TSubclassOf<AUndertowTrafficManager> TrafficManagerClass;
};
