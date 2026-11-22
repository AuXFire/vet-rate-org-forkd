// Shared types and the one place where simulation units become Unreal units.
//
// Simulation: meters, radians, x east / y north, headings counter-clockwise from east, measured from an
//             anchor on the globe that follows the player.
// Unreal:     centimeters, degrees, X east / Y south / Z up, yaw clockwise seen from above. This is what
//             Cesium for Unreal uses at its georeference origin, so road data lines up with a streamed Earth.
//
// The level's origin and the simulation's anchor are usually different places on the globe. Conversions go
// through a pair of frame maps that UUndertowSimSubsystem keeps up to date; they are the identity until then.
#pragma once

#include "CoreMinimal.h"
#include "Sim/UtGeo.h"
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
	struct FFrames
	{
		ut::FrameMap SimToLevel;
		ut::FrameMap LevelToSim;
	};
	// One set for the process: there is one simulation per game world. Defined in UndertowSimSubsystem.cpp.
	UNDERTOW_API FFrames& Frames();

	// Simulation position -> level location, and back.
	inline FVector ToWorld(ut::Vec2 P, double ZCm = 0.0)
	{
		double East = 0.0, North = 0.0;
		Frames().SimToLevel.point(static_cast<double>(P.x), static_cast<double>(P.y), East, North);
		return FVector(East * 100.0, -North * 100.0, ZCm);
	}
	inline ut::Vec2 ToSim(const FVector& V)
	{
		double East = 0.0, North = 0.0;
		Frames().LevelToSim.point(V.X * 0.01, -V.Y * 0.01, East, North);
		return ut::Vec2(static_cast<float>(East), static_cast<float>(North));
	}
	inline ut::Vec2 VelocityToSim(const FVector& V)
	{
		return Frames().LevelToSim.direction(ut::Vec2(static_cast<float>(V.X * 0.01), static_cast<float>(-V.Y * 0.01)));
	}
	inline float HeadingToYaw(float HeadingRadians) { return -FMath::RadiansToDegrees(Frames().SimToLevel.heading(HeadingRadians)); }
	inline float YawToHeading(float YawDegrees) { return Frames().LevelToSim.heading(FMath::DegreesToRadians(-YawDegrees)); }

	// Axis and unit change only, for data that is already in the level's own frame (the city blockout).
	inline FVector LevelMetersToWorld(ut::Vec2 P, double ZCm = 0.0) { return FVector(static_cast<double>(P.x) * 100.0, static_cast<double>(P.y) * -100.0, ZCm); }
	inline float LevelHeadingToYaw(float HeadingRadians) { return -FMath::RadiansToDegrees(HeadingRadians); }
	inline FString ToFString(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }
	inline std::string ToStd(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
}
