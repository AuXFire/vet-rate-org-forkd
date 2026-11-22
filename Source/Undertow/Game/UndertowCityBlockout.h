// Builds a walkable, drivable grey-box of Port Meridian from Content/Data/city.json: ground per district,
// a slab per road, a box per building, water below. Drop one into an empty level and press "Rebuild" in the
// Details panel. It exists to test gameplay at city scale before any real environment art.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UndertowCityBlockout.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

UCLASS()
class UNDERTOW_API AUndertowCityBlockout : public AActor
{
	GENERATED_BODY()

public:
	AUndertowCityBlockout();

	// Clears and regenerates every instance from city.json.
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Undertow")
	void Rebuild();

	// A 1 m cube with its pivot at the centre (the engine's basic Cube by default).
	UPROPERTY(EditAnywhere, Category = "Undertow")
	TObjectPtr<UStaticMesh> BoxMesh;
	UPROPERTY(EditAnywhere, Category = "Undertow")
	TObjectPtr<UMaterialInterface> GroundMaterial;
	UPROPERTY(EditAnywhere, Category = "Undertow")
	TObjectPtr<UMaterialInterface> RoadMaterial;
	UPROPERTY(EditAnywhere, Category = "Undertow")
	TObjectPtr<UMaterialInterface> BuildingMaterial;
	UPROPERTY(EditAnywhere, Category = "Undertow")
	TObjectPtr<UMaterialInterface> WaterMaterial;

	UPROPERTY(VisibleAnywhere, Category = "Undertow")
	TObjectPtr<UInstancedStaticMeshComponent> Ground;
	UPROPERTY(VisibleAnywhere, Category = "Undertow")
	TObjectPtr<UInstancedStaticMeshComponent> Roads;
	UPROPERTY(VisibleAnywhere, Category = "Undertow")
	TObjectPtr<UInstancedStaticMeshComponent> Buildings;
	UPROPERTY(VisibleAnywhere, Category = "Undertow")
	TObjectPtr<UInstancedStaticMeshComponent> Water;

private:
	UInstancedStaticMeshComponent* MakeLayer(const TCHAR* Name, bool bCollides);
};
