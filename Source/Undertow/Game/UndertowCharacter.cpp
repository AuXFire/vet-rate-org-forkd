#include "Game/UndertowCharacter.h"

#include "Game/UndertowSimSubsystem.h"
#include "Game/UndertowVehicle.h"
#include "Game/UndertowWeaponComponent.h"
#include "Sim/UtSim.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "TimerManager.h"

AUndertowCharacter::AUndertowCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	GetCapsuleComponent()->InitCapsuleSize(38.0f, 92.0f);

	// The camera turns with the mouse; the body turns to face the direction of travel.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 540.0f, 0.0f);
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	GetCharacterMovement()->JumpZVelocity = 520.0f;
	GetCharacterMovement()->AirControl = 0.3f;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 360.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 55.0f, 70.0f);
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	Weapon = CreateDefaultSubobject<UUndertowWeaponComponent>(TEXT("Weapon"));
}

void AUndertowCharacter::BeginPlay()
{
	Super::BeginPlay();
	Health = MaxHealth;
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AUndertowCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
	// Each pawn brings its own input mappings, so walking and driving controls never overlap.
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

void AUndertowCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		return;
	}
	if (MoveAction) { Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AUndertowCharacter::Move); }
	if (LookAction) { Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AUndertowCharacter::Look); }
	if (JumpAction)
	{
		Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	}
	if (SprintAction)
	{
		Input->BindAction(SprintAction, ETriggerEvent::Started, this, &AUndertowCharacter::StartSprint);
		Input->BindAction(SprintAction, ETriggerEvent::Completed, this, &AUndertowCharacter::StopSprint);
	}
	if (InteractAction) { Input->BindAction(InteractAction, ETriggerEvent::Started, this, &AUndertowCharacter::Interact); }
	if (FireAction) { Input->BindAction(FireAction, ETriggerEvent::Triggered, this, &AUndertowCharacter::Fire); }
}

void AUndertowCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller || IsDown())
	{
		return;
	}
	const FRotator YawOnly(0.0, Controller->GetControlRotation().Yaw, 0.0);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y), Axis.X);
}

void AUndertowCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AUndertowCharacter::StartSprint() { GetCharacterMovement()->MaxWalkSpeed = SprintSpeed; }
void AUndertowCharacter::StopSprint() { GetCharacterMovement()->MaxWalkSpeed = WalkSpeed; }

void AUndertowCharacter::Fire()
{
	if (Weapon && !IsDown())
	{
		Weapon->TryFire(FollowCamera->GetComponentLocation(), FollowCamera->GetForwardVector());
	}
}

AUndertowVehicle* AUndertowCharacter::FindVehicleInRange() const
{
	AUndertowVehicle* Best = nullptr;
	double BestDistSq = FMath::Square(static_cast<double>(EnterVehicleRange));
	for (TActorIterator<AUndertowVehicle> It(GetWorld()); It; ++It)
	{
		const double DistSq = FVector::DistSquared(It->GetActorLocation(), GetActorLocation());
		if (DistSq <= BestDistSq && !It->HasDriver())
		{
			BestDistSq = DistSq;
			Best = *It;
		}
	}
	return Best;
}

void AUndertowCharacter::Interact()
{
	if (IsDown())
	{
		return;
	}
	if (AUndertowVehicle* Vehicle = FindVehicleInRange())
	{
		Vehicle->EnterAsDriver(this);
	}
}

void AUndertowCharacter::ExitedVehicle(const FVector& Location, const FRotator& Facing)
{
	SetActorLocationAndRotation(Location, Facing, false, nullptr, ETeleportType::TeleportPhysics);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
}

float AUndertowCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	const float Applied = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (IsDown() || Applied <= 0.0f)
	{
		return 0.0f;
	}
	Health = FMath::Max(0.0f, Health - Applied);
	if (IsDown())
	{
		GetCharacterMovement()->DisableMovement();
		if (UUndertowSimSubsystem* Sim = GetWorld()->GetSubsystem<UUndertowSimSubsystem>())
		{
			Sim->NotifyMission(TEXT("player_down"), FString());
		}
		GetWorldTimerManager().SetTimer(RespawnTimer, this, &AUndertowCharacter::Respawn, RespawnDelay, false);
	}
	return Applied;
}

void AUndertowCharacter::Respawn()
{
	// Wake up outside the nearest hospital with heat cleared, as the genre expects.
	FVector Location = GetActorLocation();
	if (UUndertowSimSubsystem* Subsystem = GetWorld()->GetSubsystem<UUndertowSimSubsystem>())
	{
		if (ut::Simulation* Sim = Subsystem->GetSim())
		{
			if (const ut::Poi* Hospital = Sim->city.nearestPoi("hospital", UndertowConvert::ToSim(Location)))
			{
				Location = UndertowConvert::ToWorld(Hospital->pos, Location.Z + 100.0);
			}
			ut::EventQueue Discard;
			Sim->heat.clear(Discard);
		}
	}
	Health = MaxHealth;
	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
}
