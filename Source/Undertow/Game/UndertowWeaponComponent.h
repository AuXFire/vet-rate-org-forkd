// A simple hitscan weapon: one trace per shot, damage on hit, and the shot reported to the simulation as
// gunfire together with how many bystanders were close enough to hear it.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UndertowWeaponComponent.generated.h"

UCLASS(ClassGroup = (Undertow), meta = (BlueprintSpawnableComponent))
class UNDERTOW_API UUndertowWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Values mirror one entry of Content/Data/weapons.json; set them when the player equips a weapon.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	float Damage = 24.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	float RoundsPerMinute = 330.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	float RangeMeters = 55.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	float NoiseMeters = 70.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	int32 MagazineSize = 12;
	UPROPERTY(BlueprintReadWrite, Category = "Undertow")
	int32 RoundsInMagazine = 12;
	UPROPERTY(BlueprintReadWrite, Category = "Undertow")
	int32 ReserveRounds = 48;

	// Fires if the fire-rate timer and ammunition allow it. Returns true if a shot went off.
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	bool TryFire(FVector Origin, FVector Direction);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void Reload();

	// For muzzle flash, sound and impact effects in Blueprint.
	UFUNCTION(BlueprintImplementableEvent, Category = "Undertow")
	void OnFired(FVector Origin, FVector ImpactPoint, bool bHitSomething);

private:
	double LastShotTime = -1000.0;
};
