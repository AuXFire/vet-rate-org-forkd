#include "Game/UndertowVehicle.h"

#include "Game/UndertowCharacter.h"
#include "Game/UndertowSimSubsystem.h"

#include "Camera/CameraComponent.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"

AUndertowVehicle::AUndertowVehicle()
{
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 0.0f, 180.0f);
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
}

float AUndertowVehicle::GetSpeed() const
{
	const UChaosVehicleMovementComponent* Movement = GetVehicleMovementComponent();
	return Movement ? Movement->GetForwardSpeed() * 0.01f : 0.0f;
}

void AUndertowVehicle::PawnClientRestart()
{
	Super::PawnClientRestart();
	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Input->ClearAllMappings();
			if (MappingContext)
			{
				Input->AddMappingContext(MappingContext, 0);
			}
		}
	}
}

void AUndertowVehicle::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		return;
	}
	if (ThrottleAction)
	{
		Input->BindAction(ThrottleAction, ETriggerEvent::Triggered, this, &AUndertowVehicle::Throttle);
		Input->BindAction(ThrottleAction, ETriggerEvent::Completed, this, &AUndertowVehicle::Throttle);
	}
	if (BrakeAction)
	{
		Input->BindAction(BrakeAction, ETriggerEvent::Triggered, this, &AUndertowVehicle::Brake);
		Input->BindAction(BrakeAction, ETriggerEvent::Completed, this, &AUndertowVehicle::Brake);
	}
	if (SteerAction)
	{
		Input->BindAction(SteerAction, ETriggerEvent::Triggered, this, &AUndertowVehicle::Steer);
		Input->BindAction(SteerAction, ETriggerEvent::Completed, this, &AUndertowVehicle::Steer);
	}
	if (HandbrakeAction)
	{
		Input->BindAction(HandbrakeAction, ETriggerEvent::Started, this, &AUndertowVehicle::HandbrakeOn);
		Input->BindAction(HandbrakeAction, ETriggerEvent::Completed, this, &AUndertowVehicle::HandbrakeOff);
	}
	if (LookAction) { Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AUndertowVehicle::Look); }
	if (ExitAction) { Input->BindAction(ExitAction, ETriggerEvent::Started, this, &AUndertowVehicle::ExitDriver); }
}

void AUndertowVehicle::Throttle(const FInputActionValue& Value) { GetVehicleMovementComponent()->SetThrottleInput(Value.Get<float>()); }
void AUndertowVehicle::Brake(const FInputActionValue& Value) { GetVehicleMovementComponent()->SetBrakeInput(Value.Get<float>()); }
void AUndertowVehicle::Steer(const FInputActionValue& Value) { GetVehicleMovementComponent()->SetSteeringInput(Value.Get<float>()); }
void AUndertowVehicle::HandbrakeOn() { GetVehicleMovementComponent()->SetHandbrakeInput(true); }
void AUndertowVehicle::HandbrakeOff() { GetVehicleMovementComponent()->SetHandbrakeInput(false); }

void AUndertowVehicle::Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AUndertowVehicle::ReleasePedals()
{
	UChaosVehicleMovementComponent* Movement = GetVehicleMovementComponent();
	Movement->SetThrottleInput(0.0f);
	Movement->SetSteeringInput(0.0f);
	Movement->SetBrakeInput(0.0f);
	Movement->SetHandbrakeInput(true);   // a parked car stays put
}

void AUndertowVehicle::EnterAsDriver(AUndertowCharacter* Character)
{
	AController* PlayerController = Character ? Character->GetController() : nullptr;
	if (!PlayerController || Driver)
	{
		return;
	}
	if (!bOwnedByPlayer)
	{
		bOwnedByPlayer = true;
		if (UUndertowSimSubsystem* Sim = GetWorld()->GetSubsystem<UUndertowSimSubsystem>())
		{
			// Witness counting is the weapon component's job for gunfire; for theft assume one passer-by.
			Sim->ReportCrime(EUndertowCrime::VehicleTheft, GetActorLocation(), false, 1);
		}
	}
	Driver = Character;
	Character->SetActorHiddenInGame(true);
	Character->SetActorEnableCollision(false);
	Character->AttachToActor(this, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	GetVehicleMovementComponent()->SetHandbrakeInput(false);
	PlayerController->Possess(this);
}

void AUndertowVehicle::ExitDriver()
{
	AController* PlayerController = GetController();
	AUndertowCharacter* Character = Driver;
	if (!PlayerController || !Character)
	{
		return;
	}
	ReleasePedals();
	Driver = nullptr;
	Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	// Step out on the driver's side (left), facing the way the car points.
	const FVector Side = GetActorLocation() - GetActorRightVector() * 220.0 + FVector(0.0, 0.0, 60.0);
	Character->ExitedVehicle(Side, FRotator(0.0, GetActorRotation().Yaw, 0.0));
	PlayerController->Possess(Character);
}
