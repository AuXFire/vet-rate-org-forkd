#include "Game/UndertowAIDriverComponent.h"

#include "Game/UndertowTypes.h"

#include "ChaosVehicleMovementComponent.h"
#include "WheeledVehiclePawn.h"

UUndertowAIDriverComponent::UUndertowAIDriverComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UUndertowAIDriverComponent::SetPath(const TArray<FVector>& WorldPoints)
{
	std::vector<ut::Vec2> Points;
	Points.reserve(static_cast<size_t>(WorldPoints.Num()));
	for (const FVector& P : WorldPoints)
	{
		Points.push_back(UndertowConvert::ToSim(P));
	}
	Driver.setPath(Points);
}

void UUndertowAIDriverComponent::ClearPath()
{
	Driver.setPath(std::vector<ut::Vec2>());
}

void UUndertowAIDriverComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AWheeledVehiclePawn* Vehicle = Cast<AWheeledVehiclePawn>(GetOwner());
	UChaosVehicleMovementComponent* Movement = Vehicle ? Vehicle->GetVehicleMovementComponent() : nullptr;
	if (!Movement || Vehicle->IsPlayerControlled())
	{
		return;
	}
	ut::VehicleState State;
	State.pos = UndertowConvert::ToSim(Vehicle->GetActorLocation());
	State.heading = UndertowConvert::YawToHeading(static_cast<float>(Vehicle->GetActorRotation().Yaw));
	State.speed = FMath::Max(0.0f, Movement->GetForwardSpeed() * 0.01f);

	const ut::DriveControls Controls = Driver.update(State, CruiseSpeed);
	Movement->SetThrottleInput(Controls.throttle);
	Movement->SetBrakeInput(Controls.brake);
	// The simulation steers positive = counter-clockwise (left); Unreal vehicles steer positive = right.
	Movement->SetSteeringInput(-Controls.steer);
	Movement->SetHandbrakeInput(Controls.handbrake);
}
