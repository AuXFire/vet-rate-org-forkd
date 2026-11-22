// A drivable car on Chaos vehicle physics. The mesh, wheels and engine setup come from a Blueprint subclass
// (start from Epic's Vehicle Template assets); this class adds input, getting in and out, and the tag that
// missions match against.
#pragma once

#include "CoreMinimal.h"
#include "WheeledVehiclePawn.h"
#include "UndertowVehicle.generated.h"

class AUndertowCharacter;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
struct FInputActionValue;

UCLASS()
class UNDERTOW_API AUndertowVehicle : public AWheeledVehiclePawn
{
	GENERATED_BODY()

public:
	AUndertowVehicle();

	virtual void PawnClientRestart() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// What missions call this vehicle: "van", "coupe", "truck", ... (see Content/Data/vehicles.json).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Undertow")
	FName VehicleTag = TEXT("sedan");
	// Taking a parked car that is not yours is vehicle theft the first time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Undertow")
	bool bOwnedByPlayer = false;

	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void EnterAsDriver(AUndertowCharacter* Character);
	UFUNCTION(BlueprintCallable, Category = "Undertow")
	void ExitDriver();
	UFUNCTION(BlueprintPure, Category = "Undertow")
	bool HasDriver() const { return Driver != nullptr || IsPawnControlled(); }
	// Meters per second, positive forwards.
	UFUNCTION(BlueprintPure, Category = "Undertow")
	float GetSpeed() const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> ThrottleAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> BrakeAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> SteerAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> HandbrakeAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> ExitAction;

private:
	void Throttle(const FInputActionValue& Value);
	void Brake(const FInputActionValue& Value);
	void Steer(const FInputActionValue& Value);
	void HandbrakeOn();
	void HandbrakeOff();
	void Look(const FInputActionValue& Value);
	void ReleasePedals();

	UPROPERTY()
	TObjectPtr<AUndertowCharacter> Driver;
};
