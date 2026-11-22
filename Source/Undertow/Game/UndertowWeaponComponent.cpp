#include "Game/UndertowWeaponComponent.h"

#include "Game/UndertowSimSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"

bool UUndertowWeaponComponent::TryFire(FVector Origin, FVector Direction)
{
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner || RoundsPerMinute <= 0.0f)
	{
		return false;
	}
	const double Now = World->GetTimeSeconds();
	if (Now - LastShotTime < 60.0 / static_cast<double>(RoundsPerMinute))
	{
		return false;
	}
	if (MagazineSize > 0 && RoundsInMagazine <= 0)
	{
		Reload();
		return false;
	}
	LastShotTime = Now;
	if (MagazineSize > 0)
	{
		--RoundsInMagazine;
	}

	const FVector Dir = Direction.GetSafeNormal();
	const FVector End = Origin + Dir * (static_cast<double>(RangeMeters) * 100.0);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(UndertowWeaponTrace), true, Owner);
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, Origin, End, ECC_Visibility, Params);
	if (bHit && Hit.GetActor())
	{
		APawn* OwnerPawn = Cast<APawn>(Owner);
		UGameplayStatics::ApplyPointDamage(Hit.GetActor(), Damage, Dir, Hit, OwnerPawn ? OwnerPawn->GetController() : nullptr, Owner, nullptr);
	}
	OnFired(Origin, bHit ? FVector(Hit.ImpactPoint) : End, bHit);

	// Everyone within earshot is a potential witness. Whether an officer saw it is the police manager's
	// call (it feeds line-of-sight to the simulation every frame), so report it as civilian-witnessed here.
	int32 Witnesses = 0;
	const double NoiseSq = FMath::Square(static_cast<double>(NoiseMeters) * 100.0);
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		if (*It != Owner && FVector::DistSquared(It->GetActorLocation(), Origin) <= NoiseSq)
		{
			++Witnesses;
		}
	}
	if (UUndertowSimSubsystem* Sim = World->GetSubsystem<UUndertowSimSubsystem>())
	{
		Sim->ReportCrime(EUndertowCrime::Gunfire, Origin, false, Witnesses);
	}
	return true;
}

void UUndertowWeaponComponent::Reload()
{
	const int32 Needed = MagazineSize - RoundsInMagazine;
	const int32 Loaded = FMath::Min(Needed, ReserveRounds);
	if (Loaded > 0)
	{
		RoundsInMagazine += Loaded;
		ReserveRounds -= Loaded;
	}
}
