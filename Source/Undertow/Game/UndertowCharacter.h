// The on-foot player: third-person camera, movement, health, entering vehicles.
// Input assets (mapping context and actions) are assigned in a Blueprint subclass; see Docs/PORTING.md.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "UndertowCharacter.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
class UUndertowWeaponComponent;
class AUndertowVehicle;
struct FInputActionValue;

UCLASS()
class UNDERTOW_API AUndertowCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AUndertowCharacter();

	virtual void PawnClientRestart() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	UFUNCTION(BlueprintPure, Category = "Undertow")
	float GetHealth() const { return Health; }
	UFUNCTION(BlueprintPure, Category = "Undertow")
	bool IsDown() const { return Health <= 0.0f; }
	// Called by a vehicle when the player gets out, to put the character back in the world beside it.
	void ExitedVehicle(const FVector& Location, const FRotator& Facing);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Undertow")
	TObjectPtr<UUndertowWeaponComponent> Weapon;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> SprintAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> InteractAction;
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float MaxHealth = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float WalkSpeed = 420.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float SprintSpeed = 680.0f;
	// How close (cm) a vehicle must be to get in.
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float EnterVehicleRange = 320.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Undertow")
	float RespawnDelay = 4.0f;

protected:
	virtual void BeginPlay() override;

private:
	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void StartSprint();
	void StopSprint();
	void Interact();
	void Fire();
	void Respawn();
	AUndertowVehicle* FindVehicleInRange() const;

	float Health = 100.0f;
	FTimerHandle RespawnTimer;
};
