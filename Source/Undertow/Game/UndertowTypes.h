// Shared types and the one place where simulation units become Unreal units.
//
// Simulation: meters, radians, x east / y north, headings counter-clockwise from east.
// Unreal:     centimeters, degrees, X north / Y east (so the level matches Docs/map.svg seen from above),
//             yaw clockwise from north.
#pragma once

#include "CoreMinimal.h"
#include "Sim/UtMath.h"

#include <string>

#include "UndertowTypes.generated.h"

// Must stay in the same order as ut::Crime (checked by static_assert in UndertowSimSubsystem.cpp).
UENUM(BlueprintType)
enum class EUndertowCrime : uint8
{
	Trespass,
	RecklessDriving,
	PropertyDamage,
	VehicleTheft,
	Assault,
	Robbery,
	Gunfire,
	VehicularAssault,
	AssaultOnOfficer
};

namespace UndertowConvert
{
	inline FVector ToWorld(ut::Vec2 P, double ZCm = 0.0) { return FVector(static_cast<double>(P.y) * 100.0, static_cast<double>(P.x) * 100.0, ZCm); }
	inline ut::Vec2 ToSim(const FVector& V) { return ut::Vec2(static_cast<float>(V.Y * 0.01), static_cast<float>(V.X * 0.01)); }
	inline float HeadingToYaw(float HeadingRadians) { return 90.0f - FMath::RadiansToDegrees(HeadingRadians); }
	inline float YawToHeading(float YawDegrees) { return FMath::DegreesToRadians(90.0f - YawDegrees); }
	inline FString ToFString(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }
	inline std::string ToStd(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
}
