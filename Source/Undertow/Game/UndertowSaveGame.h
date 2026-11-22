#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "UndertowSaveGame.generated.h"

// The simulation serialises itself to JSON (see ut::Simulation::save); this just carries that text to disk.
UCLASS()
class UNDERTOW_API UUndertowSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString SimulationJson;
};
