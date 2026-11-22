#include "Game/UndertowCityBlockout.h"

#include "Undertow.h"
#include "Game/UndertowTypes.h"
#include "Sim/UtJson.h"
#include "Sim/UtRoadGraph.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

AUndertowCityBlockout::AUndertowCityBlockout()
{
	PrimaryActorTick.bCanEverTick = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		BoxMesh = Cube.Object;
	}
	Ground = MakeLayer(TEXT("Ground"), true);
	SetRootComponent(Ground);
	Roads = MakeLayer(TEXT("Roads"), true);
	Buildings = MakeLayer(TEXT("Buildings"), true);
	Water = MakeLayer(TEXT("Water"), false);
}

UInstancedStaticMeshComponent* AUndertowCityBlockout::MakeLayer(const TCHAR* Name, bool bCollides)
{
	UInstancedStaticMeshComponent* Layer = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
	Layer->SetMobility(EComponentMobility::Static);
	Layer->SetCollisionProfileName(bCollides ? TEXT("BlockAll") : TEXT("NoCollision"));
	if (RootComponent && Layer != RootComponent)
	{
		Layer->SetupAttachment(RootComponent);
	}
	return Layer;
}

void AUndertowCityBlockout::Rebuild()
{
	const FString Path = FPaths::ProjectContentDir() / TEXT("Data") / TEXT("city.json");
	FString Text;
	ut::Json City;
	std::string Error;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !ut::Json::parse(UndertowConvert::ToStd(Text), City, &Error))
	{
		UE_LOG(LogUndertow, Error, TEXT("Blockout: cannot load %s (%s)"), *Path, *UndertowConvert::ToFString(Error));
		return;
	}

	struct FLayer
	{
		UInstancedStaticMeshComponent* Component;
		UMaterialInterface* Material;
	};
	for (const FLayer& Layer : {FLayer{Ground, GroundMaterial}, FLayer{Roads, RoadMaterial}, FLayer{Buildings, BuildingMaterial}, FLayer{Water, WaterMaterial}})
	{
		Layer.Component->ClearInstances();
		Layer.Component->SetStaticMesh(BoxMesh);
		if (Layer.Material)
		{
			Layer.Component->SetMaterial(0, Layer.Material);
		}
	}

	// Instances are added in this actor's local space, so the whole city moves with the actor.
	// A box of size (along X, along Y, height) meters centred at `Centre`, top surface at TopCm. With no yaw,
	// X is east-west and Y is north-south. City data is in the city's own frame, which is the level's frame.
	auto AddBox = [](UInstancedStaticMeshComponent* Layer, ut::Vec2 Centre, float SizeX, float SizeY, float Height, double TopCm, float YawDegrees)
	{
		const FVector Location = UndertowConvert::LevelMetersToWorld(Centre, TopCm - static_cast<double>(Height) * 50.0);
		Layer->AddInstance(FTransform(FRotator(0.0, YawDegrees, 0.0), Location, FVector(SizeX, SizeY, Height)));
	};

	// Ground: one slab per district, top at 0.
	const ut::Json& Districts = City["districts"];
	for (int32 i = 0; i < Districts.size(); ++i)
	{
		const ut::Json& Poly = Districts.at(i)["poly"];
		const float X0 = Poly.at(0).at(0).asFloat(), Y0 = Poly.at(0).at(1).asFloat();
		const float X1 = Poly.at(2).at(0).asFloat(), Y1 = Poly.at(2).at(1).asFloat();
		AddBox(Ground, ut::Vec2((X0 + X1) * 0.5f, (Y0 + Y1) * 0.5f), X1 - X0, Y1 - Y0, 4.0f, 0.0, 0.0f);
	}

	// Water: one sheet under everything, 1.5 m below street level, so the river and bay read as water.
	const ut::Json& Bounds = City["meta"]["bounds"];
	const float BX0 = Bounds.at(0).asFloat(), BY0 = Bounds.at(1).asFloat(), BX1 = Bounds.at(2).asFloat(), BY1 = Bounds.at(3).asFloat();
	AddBox(Water, ut::Vec2((BX0 + BX1) * 0.5f, (BY0 + BY1) * 0.5f), BX1 - BX0, BY1 - BY0, 1.0f, -150.0, 0.0f);

	// Roads: a thin slab per road, 2 cm proud of the ground so it never z-fights. Bridges get a thick deck.
	const ut::Json& Nodes = City["roads"]["nodes"];
	const ut::Json& Edges = City["roads"]["edges"];
	const int32 BridgeKind = static_cast<int32>(ut::RoadKind::Bridge);
	for (int32 i = 0; i < Edges.size(); ++i)
	{
		const ut::Json& E = Edges.at(i);
		const ut::Json& A = Nodes.at(E.at(0).asInt());
		const ut::Json& B = Nodes.at(E.at(1).asInt());
		const ut::Vec2 PA(A.at(0).asFloat(), A.at(1).asFloat()), PB(B.at(0).asFloat(), B.at(1).asFloat());
		const ut::Vec2 Delta = PB - PA;
		const float Width = static_cast<float>(E.at(2).asInt(1)) * 2.0f * ut::RoadGraph::kLaneWidth + 1.0f;   // both directions + kerbs
		const float Thickness = E.at(4).asInt() == BridgeKind ? 1.5f : 0.2f;
		// Extend by half a width at each end so slabs overlap at junctions instead of leaving notches.
		AddBox(Roads, (PA + PB) * 0.5f, Delta.length() + Width, Width, Thickness, 2.0, UndertowConvert::LevelHeadingToYaw(Delta.angle()));
	}

	// Buildings: [centreX, centreY, sizeEast, sizeNorth, height, district].
	const ut::Json& Blocks = City["buildings"];
	for (int32 i = 0; i < Blocks.size(); ++i)
	{
		const ut::Json& B = Blocks.at(i);
		const float Height = B.at(4).asFloat();
		AddBox(Buildings, ut::Vec2(B.at(0).asFloat(), B.at(1).asFloat()), B.at(2).asFloat(), B.at(3).asFloat(), Height, static_cast<double>(Height) * 100.0, 0.0f);
	}

	UE_LOG(LogUndertow, Log, TEXT("Blockout rebuilt: %d ground slabs, %d roads, %d buildings"), Districts.size(), Edges.size(), Blocks.size());
}
