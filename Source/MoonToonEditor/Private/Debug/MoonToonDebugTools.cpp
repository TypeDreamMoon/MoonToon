// Copyright Dream Moon. All Rights Reserved.

// UMoonToonDebugSubsystem: captures and diffs, the toon actor list, the registry tables, the post process
// volumes' style presets, and validation (the T44 rules, moved here from Saved/MoonToonStyle/scripts/
// t44_validate.py so the panel and the MCP tools share them, plus the GPU self-check counts).

#include "MoonToonDebugSubsystem.h"

#include "MoonToonDebugNames.h"
#include "MoonToonToolsUserSettings.h"
#include "MoonToonDebugDefinitions.h"
#include "MoonToonActorDefinitions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/MoonToonActorComponent.h"
#include "Components/PostProcessComponent.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "Dom/JsonObject.h"
#include "Engine/MoonToonProfile.h"
#include "Engine/MoonToonStylePreset.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "LevelEditorViewport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Rendering/MoonToonDebugSnapshots.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

// -------------------------------------------------------------------------------------------------
// Captures

static FString CaptureDirectory(const FString& Requested)
{
	if (!Requested.IsEmpty())
	{
		return Requested;
	}
	const FString& Configured = UMoonToonToolsUserSettings::Get().DebugCaptureDirectory;
	return !Configured.IsEmpty() ? Configured : FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MoonToonDebug") / TEXT("captures"));
}

static bool SaveImage(EImageFormat Format, const void* Data, int64 Bytes, int32 Width, int32 Height, ERGBFormat RGBFormat, int32 BitDepth, const FString& Path)
{
	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetRaw(Data, Bytes, Width, Height, RGBFormat, BitDepth))
	{
		return false;
	}
	const TArray64<uint8> Compressed = Wrapper->GetCompressed();
	return Compressed.Num() > 0 && FFileHelper::SaveArrayToFile(Compressed, *Path);
}

static bool LoadExr(const FString& Path, TArray64<uint8>& OutRaw, int32& OutWidth, int32& OutHeight)
{
	TArray64<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *Path))
	{
		return false;
	}
	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Module.DetectImageFormat(File.GetData(), File.Num());
	const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format == EImageFormat::Invalid ? EImageFormat::EXR : Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(File.GetData(), File.Num()) || !Wrapper->GetRaw(ERGBFormat::RGBAF, 32, OutRaw))
	{
		return false;
	}
	OutWidth = (int32)Wrapper->GetWidth();
	OutHeight = (int32)Wrapper->GetHeight();
	return OutRaw.Num() >= int64(OutWidth) * OutHeight * 16;
}

FMoonToonCaptureInfo UMoonToonDebugSubsystem::CaptureSync(const FString& View, int32 Channel, int32 Frames, const FString& OutputDirectory, float TimeoutSeconds)
{
	FMoonToonCaptureInfo Out;
	FString WhyNot;
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (!IsAvailable(&WhyNot) || !Client)
	{
		Out.Error = !WhyNot.IsEmpty() ? WhyNot : FString(TEXT("no level viewport"));
		return Out;
	}

	MoonToonDebug::FCaptureRequest Request;
	Request.ViewKey = Client->ViewState.GetReference();
	const FString ViewLower = View.ToLower();
	FString ViewName = ViewLower;
	if (ViewLower == TEXT("scene.hdr") || ViewLower == TEXT("scene_hdr"))
	{
		Request.Source = MoonToonDebug::ECaptureSource::SceneHDR;
		ViewName = TEXT("scene.hdr");
	}
	else if (ViewLower == TEXT("scene.final") || ViewLower == TEXT("scene_final"))
	{
		Request.Source = MoonToonDebug::ECaptureSource::SceneFinal;
		ViewName = TEXT("scene.final");
	}
	else
	{
		const MoonToonDebug::FDebugViewInfo* ViewInfo = MoonToonDebug::FindView(View);
		if (!ViewInfo)
		{
			Out.Error = FString::Printf(TEXT("unknown view '%s' (list_debug_views, or scene.hdr / scene.final)"), *View);
			return Out;
		}
		Request.Source = MoonToonDebug::ECaptureSource::DebugView;
		Request.Selection = GetViewSettings();
		Request.Selection.View = ViewInfo->Name;
		Request.Selection.Channel = Channel;
		Request.Selection.IsolateKey = -1;
		ViewName = ViewInfo->Name;
	}

	// Warm-up frames (TSR history, streaming) before the one that is captured.
	for (int32 Frame = 1; Frame < FMath::Max(Frames, 1); ++Frame)
	{
		if (Client->Viewport->GetSizeXY().X <= 0)
		{
			break;
		}
		Client->Invalidate(false, false);
		Client->Viewport->Draw(false);
		FlushRenderingCommands();
	}

	struct FCaptureBox { bool bDone = false; MoonToonDebug::FCaptureResult Result; };
	const TSharedRef<FCaptureBox> Box = MakeShared<FCaptureBox>();
	MoonToonDebug::RequestCapture(Request, [Box](const MoonToonDebug::FCaptureResult& R) { Box->Result = R; Box->bDone = true; });
	if (!DrawAndWait([Box]() { return Box->bDone; }, 1, TimeoutSeconds))
	{
		MoonToonDebug::CancelPendingRequests();
		MoonToonDebug::FlushAndResolve();
		Out.Error = TEXT("timed out: the viewport did not render");
		return Out;
	}
	const MoonToonDebug::FCaptureResult& Result = Box->Result;
	if (!Result.bValid)
	{
		Out.Error = Result.Error;
		return Out;
	}

	const FString Directory = CaptureDirectory(OutputDirectory);
	IFileManager::Get().MakeDirectory(*Directory, true);
	FString Base = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + ViewName.Replace(TEXT("."), TEXT("_"));
	if (Request.Source == MoonToonDebug::ECaptureSource::DebugView && Channel != 0)
	{
		Base += FString::Printf(TEXT("_ch%d"), Channel);
	}
	const FString BasePath = Directory / Base;
	const int32 Width = Result.Size.X;
	const int32 Height = Result.Size.Y;
	Out.ViewName = ViewName;
	Out.Width = Width;
	Out.Height = Height;

	Out.RawPath = BasePath + TEXT(".exr");
	if (!SaveImage(EImageFormat::EXR, Result.Raw.GetData(), Result.Raw.Num() * sizeof(FLinearColor), Width, Height, ERGBFormat::RGBAF, 32, Out.RawPath))
	{
		Out.Error = TEXT("could not write the EXR");
		return Out;
	}
	if (Result.Keys.Num() == Width * Height && Width > 0)
	{
		// R = key low 16 bits, G = key high 16 bits, A = painted -- an id survives exactly.
		TArray<uint16> Pixels;
		Pixels.SetNumZeroed(Width * Height * 4);
		for (int32 It = 0; It < Result.Keys.Num(); ++It)
		{
			const uint32 Key = Result.Keys[It];
			const bool bPainted = Key != 0xFFFFFFFFu;
			Pixels[It * 4 + 0] = bPainted ? (uint16)(Key & 0xFFFFu) : 0;
			Pixels[It * 4 + 1] = bPainted ? (uint16)(Key >> 16u) : 0;
			Pixels[It * 4 + 2] = 0;
			Pixels[It * 4 + 3] = bPainted ? 0xFFFFu : 0;
		}
		Out.IdPath = BasePath + TEXT("_id.png");
		SaveImage(EImageFormat::PNG, Pixels.GetData(), Pixels.Num() * sizeof(uint16), Width, Height, ERGBFormat::RGBA, 16, Out.IdPath);
	}
	if (Result.FalseColor.Num() == Width * Height && Width > 0)
	{
		Out.FalseColorPath = BasePath + TEXT("_color.png");
		SaveImage(EImageFormat::PNG, Result.FalseColor.GetData(), Result.FalseColor.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8, Out.FalseColorPath);
	}

	// The sidecar: how to map these pixels back, and what produced them.
	TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("view"), ViewName);
	Meta->SetNumberField(TEXT("channel"), Channel);
	Meta->SetNumberField(TEXT("encoding"), Result.Encoding);
	Meta->SetNumberField(TEXT("frame"), (double)Result.FrameNumber);
	Meta->SetNumberField(TEXT("width"), Width);
	Meta->SetNumberField(TEXT("height"), Height);
	Meta->SetStringField(TEXT("pixel_space"), Request.Source == MoonToonDebug::ECaptureSource::DebugView ? TEXT("render (view rect, 0-based)") : TEXT("output (post-upscale rect, 0-based)"));
	Meta->SetStringField(TEXT("view_rect"), Result.ViewRect.ToString());
	Meta->SetStringField(TEXT("viewport_rect"), Result.UnscaledViewRect.ToString());
	Meta->SetStringField(TEXT("output_rect"), Result.OutputRect.ToString());
	Meta->SetNumberField(TEXT("pre_exposure"), Result.PreExposure);
	Meta->SetStringField(TEXT("camera_location"), Client->GetViewLocation().ToString());
	Meta->SetStringField(TEXT("camera_rotation"), Client->GetViewRotation().ToString());
	const MoonToonDebug::FViewSettings Settings = Request.Selection;
	Meta->SetStringField(TEXT("settings"), FString::Printf(TEXT("view=%s channel=%d bit=%d resolved=%d range=%g..%g"),
		*Settings.View, Settings.Channel, Settings.Bit, Settings.bResolved ? 1 : 0, Settings.RangeMin, Settings.RangeMax));
	Meta->SetStringField(TEXT("raw_exr"), Out.RawPath);
	Meta->SetStringField(TEXT("id_png16"), Out.IdPath);
	Meta->SetStringField(TEXT("false_color_png"), Out.FalseColorPath);
	FString MetaText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MetaText);
	FJsonSerializer::Serialize(Meta, Writer);
	Out.MetaPath = BasePath + TEXT(".json");
	FFileHelper::SaveStringToFile(MetaText, *Out.MetaPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Out.bValid = true;
	return Out;
}

FMoonToonDiffResult UMoonToonDebugSubsystem::DiffCaptures(const FString& A, const FString& B, const FString& NoiseA, const FString& NoiseB, float Threshold, const FString& MaskPath)
{
	FMoonToonDiffResult Out;
	Out.Threshold = Threshold > 0.0f ? Threshold : 0.01f;
	TArray64<uint8> RawA, RawB, RawNA, RawNB;
	int32 WA = 0, HA = 0, WB = 0, HB = 0, WN = 0, HN = 0, WN2 = 0, HN2 = 0;
	if (!LoadExr(A, RawA, WA, HA) || !LoadExr(B, RawB, WB, HB))
	{
		Out.Error = TEXT("could not read A or B (raw EXR captures)");
		return Out;
	}
	if (WA != WB || HA != HB)
	{
		Out.Error = FString::Printf(TEXT("size mismatch: %dx%d vs %dx%d"), WA, HA, WB, HB);
		return Out;
	}
	const bool bNoise = !NoiseA.IsEmpty() && !NoiseB.IsEmpty();
	if (bNoise && (!LoadExr(NoiseA, RawNA, WN, HN) || !LoadExr(NoiseB, RawNB, WN2, HN2) || WN != WA || HN != HA || WN2 != WA || HN2 != HA))
	{
		Out.Error = TEXT("could not read the noise pair, or its size differs");
		return Out;
	}
	const FLinearColor* PA = reinterpret_cast<const FLinearColor*>(RawA.GetData());
	const FLinearColor* PB = reinterpret_cast<const FLinearColor*>(RawB.GetData());
	const FLinearColor* PNA = bNoise ? reinterpret_cast<const FLinearColor*>(RawNA.GetData()) : nullptr;
	const FLinearColor* PNB = bNoise ? reinterpret_cast<const FLinearColor*>(RawNB.GetData()) : nullptr;
	const auto MaxDiff = [](const FLinearColor& X, const FLinearColor& Y)
	{
		return FMath::Max3(FMath::Abs(X.R - Y.R), FMath::Abs(X.G - Y.G), FMath::Abs(X.B - Y.B));
	};

	Out.Width = WA;
	Out.Height = HA;
	Out.NoisePixels = bNoise ? 0 : -1;
	double Sum = 0.0;
	FIntPoint Min(INT32_MAX, INT32_MAX), Max(INT32_MIN, INT32_MIN);
	TArray<FColor> Mask;
	Mask.Init(FColor::Black, WA * HA);
	for (int32 Y = 0; Y < HA; ++Y)
	{
		for (int32 X = 0; X < WA; ++X)
		{
			const int32 It = Y * WA + X;
			const float D = MaxDiff(PA[It], PB[It]);
			Sum += D;
			Out.MaxDiff = FMath::Max(Out.MaxDiff, D);
			const bool bChanged = D > Out.Threshold;
			const bool bNoisy = bNoise && MaxDiff(PNA[It], PNB[It]) > Out.Threshold;
			Out.ChangedPixels += bChanged ? 1 : 0;
			if (bNoise)
			{
				Out.NoisePixels += bNoisy ? 1 : 0;
			}
			if (bChanged && !bNoisy)
			{
				++Out.StablePixels;
				Mask[It] = FColor::Magenta;
				Min = Min.ComponentMin(FIntPoint(X, Y));
				Max = Max.ComponentMax(FIntPoint(X + 1, Y + 1));
			}
			else if (bNoisy)
			{
				Mask[It] = FColor(40, 40, 40);
			}
		}
	}
	Out.MeanDiff = WA * HA > 0 ? (float)(Sum / (WA * HA)) : 0.0f;
	if (Out.StablePixels > 0)
	{
		Out.BoundsMin = Min;
		Out.BoundsMax = Max;
	}
	// The regression harness's rule (regress_cmp.py): a change no larger than the noise is no change.
	const float Allowance = bNoise ? 1.5f * (float)Out.NoisePixels + 50.0f : 50.0f;
	Out.Verdict = (float)Out.StablePixels <= Allowance ? TEXT("SAME") : TEXT("DIFF");
	Out.MaskPath = !MaskPath.IsEmpty() ? MaskPath : FPaths::GetPath(B) / (FPaths::GetBaseFilename(B) + TEXT("_diff.png"));
	SaveImage(EImageFormat::PNG, Mask.GetData(), Mask.Num() * sizeof(FColor), WA, HA, ERGBFormat::BGRA, 8, Out.MaskPath);
	Out.bValid = true;

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("verdict"), Out.Verdict);
	Root->SetNumberField(TEXT("threshold"), Out.Threshold);
	Root->SetNumberField(TEXT("max_diff"), Out.MaxDiff);
	Root->SetNumberField(TEXT("mean_diff"), Out.MeanDiff);
	Root->SetNumberField(TEXT("changed_pixels"), Out.ChangedPixels);
	Root->SetNumberField(TEXT("noise_pixels"), Out.NoisePixels);
	Root->SetNumberField(TEXT("stable_pixels"), Out.StablePixels);
	Root->SetStringField(TEXT("bounds"), FString::Printf(TEXT("%d %d %d %d"), Out.BoundsMin.X, Out.BoundsMin.Y, Out.BoundsMax.X, Out.BoundsMax.Y));
	Root->SetStringField(TEXT("mask"), Out.MaskPath);
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out.Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Toon materials and actors

static bool IsToonMaterial(const UMaterialInterface* Material)
{
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base)
	{
		return false;
	}
	if (Base->GetName().Contains(TEXT("MoonToon")) || Base->GetPathName().StartsWith(TEXT("/MoonToon/")))
	{
		return true;
	}
	return Base->GetShadingModels().HasShadingModel(MSM_Toon);
}

static void GatherToonMaterials(AActor* Actor, TArray<UMaterialInterface*>& Out)
{
	TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		for (int32 Slot = 0; Slot < Primitive->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Material = Primitive->GetMaterial(Slot);
			if (IsToonMaterial(Material))
			{
				Out.AddUnique(Material);
			}
		}
	}
}

TArray<FMoonToonDebugActorInfo> UMoonToonDebugSubsystem::ListToonActors(bool bWithGpuData, float TimeoutSeconds)
{
	TArray<FMoonToonDebugActorInfo> Out;
	UWorld* World = GetTargetWorld();
	if (!World)
	{
		return Out;
	}
	RefreshNameCaches(true);

	// GPU data: the actor table as this frame holds it, and how many pixels each id covers.
	MoonToonDebug::FActorTableResult Table;
	TMap<int32, int32> PixelsById;
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (bWithGpuData && Client && IsAvailable())
	{
		struct FTableBox { bool bDone = false; MoonToonDebug::FActorTableResult Result; };
		const TSharedRef<FTableBox> Box = MakeShared<FTableBox>();
		MoonToonDebug::RequestActorTable(Client->ViewState.GetReference(), [Box](const MoonToonDebug::FActorTableResult& R) { Box->Result = R; Box->bDone = true; });
		if (DrawAndWait([Box]() { return Box->bDone; }, 1, TimeoutSeconds))
		{
			Table = Box->Result;
		}
		const FMoonToonRegionStats Stats = RegionStatsSync(TEXT("actor"), 0, -1, false, FIntPoint::ZeroValue, FIntPoint::ZeroValue, TimeoutSeconds);
		for (const FMoonToonRegionValue& Value : Stats.Values)
		{
			PixelsById.Add((int32)Value.Key, Value.PixelCount);
		}
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		UMoonToonActorComponent* Component = Actor->FindComponentByClass<UMoonToonActorComponent>();
		TArray<UMaterialInterface*> Materials;
		GatherToonMaterials(Actor, Materials);
		if (!Component && Materials.Num() == 0)
		{
			continue;
		}
		FMoonToonDebugActorInfo& Info = Out.AddDefaulted_GetRef();
		Info.ActorLabel = Actor->GetActorNameOrLabel();
		Info.ActorPath = Actor->GetPathName();
		Info.bHasComponent = Component != nullptr;
		Info.ActorId = Component ? Component->GetToonActorId() : 0;
		for (UMaterialInterface* Material : Materials)
		{
			Info.Materials.Add(Material->GetPathName());
		}
		if (!Component)
		{
			Info.Warnings.Add(TEXT("toon materials but no MoonToonActorComponent: no toon actor id (single-layer clipping, face overlay gates, per-character style and light rig fall back)"));
		}
		else if (Info.ActorId == 0)
		{
			Info.Warnings.Add(TEXT("MoonToonActorComponent without an id (beyond the 2047 limit, or not registered)"));
		}
		if (const int32* Pixels = PixelsById.Find(Info.ActorId))
		{
			Info.PixelCount = Info.ActorId > 0 ? *Pixels : 0;
		}
		if (Table.bValid && Info.ActorId > 0 && Table.Size.X > 0)
		{
			const int32 Column = Info.ActorId % Table.Size.X;
			const int32 BlockRow = (Info.ActorId / Table.Size.X) * Table.RowsPerActor;
			bool bSentinelOk = false;
			for (int32 Row = 0; Row < Table.RowsPerActor; ++Row)
			{
				const int32 Index = (BlockRow + Row) * Table.Size.X + Column;
				if (!Table.Texels.IsValidIndex(Index))
				{
					break;
				}
				const FVector4f& V = Table.Texels[Index];
				FMoonToonDebugField& Field = Info.ActorRows.AddDefaulted_GetRef();
				Field.Section = TEXT("actor_table");
				Field.Name = MoonToonDebugNames::ActorRowName(Row);
				Field.Value = FString::Printf(TEXT("%.4f %.4f %.4f %.4f"), V.X, V.Y, V.Z, V.W);
				Field.Number = V.X;
				if (Row == MOONTOON_ACTOR_ROW_PIVOT)
				{
					bSentinelOk = FMath::RoundToInt(V.W) == Info.ActorId;
				}
			}
			if (!bSentinelOk)
			{
				Info.Warnings.Add(TEXT("the GPU actor table has no block for this id this frame (not rendered, or stale)"));
			}
		}
	}
	Out.Sort([](const FMoonToonDebugActorInfo& X, const FMoonToonDebugActorInfo& Y) { return X.ActorId == Y.ActorId ? X.ActorLabel < Y.ActorLabel : X.ActorId < Y.ActorId; });
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Tables

TArray<FMoonToonTableRow> UMoonToonDebugSubsystem::DumpConstantTable()
{
	MoonToonDebugSnapshots::FConstantTable Table;
	ENQUEUE_RENDER_COMMAND(MoonToonDebugConstantTable)([&Table](FRHICommandListImmediate&) { Table = MoonToonDebugSnapshots::GetConstantTable(); });
	FlushRenderingCommands();

	static const TCHAR* Channels[] = { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("R"), TEXT("G"), TEXT("B'"), TEXT("E"), TEXT("F"), TEXT("G'"), TEXT("W") };
	TArray<FMoonToonTableRow> Out;
	FMoonToonTableRow& Header = Out.AddDefaulted_GetRef();
	Header.Key = TEXT("table");
	Header.Columns.Add(FString::Printf(TEXT("enabled=%d capacity=%u rows=%d registrations=%llu full=%llu"),
		Table.bEnabled ? 1 : 0, Table.Capacity, Table.Rows.Num(), Table.RegistrationCount, Table.FullCount));
	for (const MoonToonDebugSnapshots::FConstantRow& Row : Table.Rows)
	{
		FMoonToonTableRow& Entry = Out.AddDefaulted_GetRef();
		Entry.Key = FString::Printf(TEXT("row %u"), Row.Row);
		Entry.Columns.Add(FString::Printf(TEXT("refs=%u lastUsed=%llu mask=0x%03X"), Row.RefCount, Row.LastUsedFrame, Row.PerPixelMask));
		for (int32 Channel = 0; Channel < Row.Values.Num() && Channel < UE_ARRAY_COUNT(Channels); ++Channel)
		{
			Entry.Columns.Add((Row.PerPixelMask & (1u << Channel)) != 0u
				? FString::Printf(TEXT("%s=*"), Channels[Channel])
				: FString::Printf(TEXT("%s=%.4f"), Channels[Channel], Row.Values[Channel]));
		}
		Entry.ObjectPaths = Row.Materials;	// "<material>#<slot>"
	}
	return Out;
}

TArray<FMoonToonTableRow> UMoonToonDebugSubsystem::DumpRampAtlas()
{
	MoonToonDebugSnapshots::FRampAtlas Atlas;
	ENQUEUE_RENDER_COMMAND(MoonToonDebugRampAtlas)([&Atlas](FRHICommandListImmediate&) { Atlas = MoonToonDebugSnapshots::GetRampAtlas(); });
	FlushRenderingCommands();

	TArray<FMoonToonTableRow> Out;
	FMoonToonTableRow& Header = Out.AddDefaulted_GetRef();
	Header.Key = TEXT("atlas");
	Header.Columns.Add(FString::Printf(TEXT("diffuse capacity=%u legacy=%u, specular capacity=%u legacy=%u"),
		Atlas.DiffuseCapacity, Atlas.DiffuseLegacyRows, Atlas.SpecularCapacity, Atlas.SpecularLegacyRows));
	for (const MoonToonDebugSnapshots::FRampSlice& Slice : Atlas.Slices)
	{
		FMoonToonTableRow& Entry = Out.AddDefaulted_GetRef();
		Entry.Key = FString::Printf(TEXT("%s %u"), Slice.bSpecular ? TEXT("specular") : TEXT("diffuse"), Slice.Slice);
		Entry.Columns.Add(Slice.Source);
		Entry.Columns.Add(FString::Printf(TEXT("row %d"), Slice.SourceRow));
		Entry.Columns.Add(Slice.SegmentLayoutCode != 0u ? FString::Printf(TEXT("segments 0x%02X (layout code %u)"), Slice.SegmentMask, Slice.SegmentLayoutCode) : FString(TEXT("no segments")));
		Entry.Columns.Add(FString::Printf(TEXT("lastUsed %llu%s"), Slice.LastUsed, Slice.bPendingBlit ? TEXT(" (pending blit)") : TEXT("")));
		Entry.ObjectPaths.Add(Slice.Source);
	}
	return Out;
}

TArray<FMoonToonTableRow> UMoonToonDebugSubsystem::DumpProfiles()
{
	RefreshNameCaches(true);
	TArray<FMoonToonTableRow> Out;
	TArray<int32> Ids;
	ProfileNames.GetKeys(Ids);
	Ids.Sort();
	for (const int32 Id : Ids)
	{
		const FString& Path = ProfileNames[Id];
		FMoonToonTableRow& Entry = Out.AddDefaulted_GetRef();
		Entry.Key = FString::Printf(TEXT("id %d"), Id);
		Entry.Columns.Add(Path);
		Entry.ObjectPaths.Add(Path);
		// The settings through reflection (the struct's StaticStruct is not exported from Engine).
		const UMoonToonProfile* Profile = FindObject<UMoonToonProfile>(nullptr, *Path);
		const FStructProperty* SettingsProperty = Profile ? CastField<FStructProperty>(Profile->GetClass()->FindPropertyByName(TEXT("Settings"))) : nullptr;
		if (SettingsProperty)
		{
			const void* Settings = SettingsProperty->ContainerPtrToValuePtr<void>(Profile);
			for (TFieldIterator<FProperty> It(SettingsProperty->Struct); It; ++It)
			{
				FString Value;
				It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Settings), nullptr, nullptr, PPF_None);
				Entry.Columns.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *Value));
			}
		}
	}
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Style layers

static void CollectOverrides(const UStruct* Struct, const void* Data, TArray<FString>& Out, int32 Depth = 0)
{
	if (!Struct || !Data || Depth > 6)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(*It))
		{
			const FString Name = Bool->GetName();
			if (Name.StartsWith(TEXT("bOverride_")) && Bool->GetPropertyValue_InContainer(Data))
			{
				Out.AddUnique(Name.RightChop(10));
			}
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(*It))
		{
			CollectOverrides(StructProperty->Struct, StructProperty->ContainerPtrToValuePtr<void>(Data), Out, Depth + 1);
		}
	}
}

TArray<FMoonToonStyleLayer> UMoonToonDebugSubsystem::GetStyleLayers() const
{
	TArray<FMoonToonStyleLayer> Out;
	UWorld* World = GetTargetWorld();
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (!World)
	{
		return Out;
	}
	const FVector Camera = Client ? Client->GetViewLocation() : FVector::ZeroVector;
	for (IInterface_PostProcessVolume& VolumeRef : World->GetPostProcessVolumeIterator())
	{
		IInterface_PostProcessVolume* Volume = &VolumeRef;
		if (!Volume)
		{
			continue;
		}
		const FPostProcessVolumeProperties Properties = Volume->GetProperties();
		if (!Properties.bIsEnabled || !Properties.Settings)
		{
			continue;
		}
		float Weight = Properties.BlendWeight;
		if (!Properties.bIsUnbound)
		{
			float Distance = 0.0f;
			const bool bInside = Volume->EncompassesPoint(Camera, 0.0f, &Distance);
			if (!bInside)
			{
				if (Properties.BlendRadius <= 0.0f || Distance >= Properties.BlendRadius)
				{
					continue;
				}
				Weight *= 1.0f - Distance / Properties.BlendRadius;
			}
		}
		for (const FWeightedBlendable& Blendable : Properties.Settings->WeightedBlendables.Array)
		{
			const UMoonToonStylePreset* Preset = Cast<UMoonToonStylePreset>(Blendable.Object);
			if (!Preset || Blendable.Weight <= 0.0f)
			{
				continue;
			}
			FMoonToonStyleLayer& Layer = Out.AddDefaulted_GetRef();
			Layer.Volume = Volume->GetDebugName();
			const UObject* VolumeObject = Cast<UObject>(Volume);
			Layer.VolumePath = VolumeObject ? VolumeObject->GetPathName() : FString();
			Layer.Priority = Properties.Priority;
			Layer.Weight = Weight * Blendable.Weight;
			Layer.PresetPath = Preset->GetPathName();
			for (const UMoonToonStylePreset* Current = Preset; Current; Current = Current->Parent)
			{
				CollectOverrides(Current->GetClass(), Current, Layer.OverriddenFields);
				if (Current->Parent == Preset)
				{
					break;
				}
			}
		}
	}
	Out.Sort([](const FMoonToonStyleLayer& X, const FMoonToonStyleLayer& Y) { return X.Priority < Y.Priority; });
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Validation

TArray<FMoonToonValidationIssue> UMoonToonDebugSubsystem::Validate(bool bWithGpuChecks)
{
	TArray<FMoonToonValidationIssue> Out;
	const auto Add = [&Out](const TCHAR* Severity, const FString& Where, const FString& Message, const FString& Object = FString(), FIntPoint Sample = FIntPoint(-1, -1))
	{
		FMoonToonValidationIssue& Issue = Out.AddDefaulted_GetRef();
		Issue.Severity = Severity;
		Issue.Where = Where;
		Issue.Message = Message;
		Issue.ObjectPath = Object;
		Issue.SamplePixel = Sample;
	};
	UWorld* World = GetTargetWorld();
	if (!World)
	{
		Add(TEXT("ERROR"), TEXT("validator"), TEXT("no world"));
		return Out;
	}

	// Level: some post process volume must set GI Intensity, or every toon ambient / indirect term is 0.
	bool bGiFound = false;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		TArray<TPair<FString, const FPostProcessSettings*>> Sources;
		if (const APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
		{
			Sources.Add({ Actor->GetActorNameOrLabel(), &Volume->Settings });
		}
		TInlineComponentArray<UPostProcessComponent*> Components(Actor);
		for (const UPostProcessComponent* Component : Components)
		{
			Sources.Add({ Actor->GetActorNameOrLabel() + TEXT(".") + Component->GetName(), &Component->Settings });
		}
		for (const TPair<FString, const FPostProcessSettings*>& Source : Sources)
		{
			if (Source.Value->bOverride_MoonGlobalIlluminationIntensity)
			{
				bGiFound = true;
				Add(Source.Value->MoonGlobalIlluminationIntensity > 0.0f ? TEXT("INFO") : TEXT("WARN"), TEXT("level"),
					FString::Printf(TEXT("PPV %s GI Intensity %.2f"), *Source.Key, Source.Value->MoonGlobalIlluminationIntensity), Actor->GetPathName());
			}
		}
	}
	if (!bGiFound)
	{
		Add(TEXT("ERROR"), TEXT("level"), TEXT("no post process volume overrides GI Intensity (default 0): toon ambient / indirect light is 0"));
	}

	// Actors and their toon materials.
	TSet<const UMaterialInterface*> Seen;
	TMap<FString, TArray<FString>> SdfTextures;	// texture path -> instances
	int32 ToonActors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		TArray<UMaterialInterface*> Materials;
		GatherToonMaterials(Actor, Materials);
		if (Materials.Num() == 0)
		{
			continue;
		}
		++ToonActors;
		if (!Actor->FindComponentByClass<UMoonToonActorComponent>())
		{
			Add(TEXT("WARN"), TEXT("actor ") + Actor->GetActorNameOrLabel(),
				FString::Printf(TEXT("toon materials (%d) but no MoonToonActorComponent: no toon actor id"), Materials.Num()), Actor->GetPathName());
		}
		for (UMaterialInterface* Material : Materials)
		{
			if (Seen.Contains(Material))
			{
				continue;
			}
			Seen.Add(Material);
			const FString Where = Material->GetPathName();
			float Roughness = 0.0f, Specular = 1.0f, ThroughHair = 0.0f;
			const bool bRough = Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
			const bool bSpecular = Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Specular")), Specular);
			if (bRough && Roughness >= 0.999f && (!bSpecular || Specular > 0.0f))
			{
				Add(TEXT("WARN"), Where, FString::Printf(TEXT("Roughness %.3f: the toon specular is 0 at Roughness 1 (Specular %.3f)"), Roughness, Specular), Where);
			}
			if (Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Through Hair Opacity")), ThroughHair) && ThroughHair > 1.0f)
			{
				Add(TEXT("WARN"), Where, FString::Printf(TEXT("Through Hair Opacity %.2f > 1 saturates the eye overlay (0.35-0.45 reads like the reference)"), ThroughHair), Where);
			}
			if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
			{
				TArray<FMaterialParameterInfo> Infos;
				TArray<FGuid> Ids;
				Instance->GetAllTextureParameterInfo(Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					const FString Key = Info.Name.ToString().Replace(TEXT(" "), TEXT("")).ToUpper();
					if (!(Key.Contains(TEXT("SDF")) || Key.Contains(TEXT("FACIALSHADOW")) || Key.Contains(TEXT("FACESHADOW")) || Key.Contains(TEXT("DISTANCEFIELD"))))
					{
						continue;
					}
					UTexture* Texture = nullptr;
					if (Instance->GetTextureParameterValue(Info, Texture) && Texture)
					{
						const FString Path = Texture->GetPathName();
						if (!Path.StartsWith(TEXT("/MoonToon/Textures/")) && !Path.StartsWith(TEXT("/Engine/")))
						{
							SdfTextures.FindOrAdd(Path).AddUnique(Where);
						}
					}
				}
			}
			if (const UMaterial* Base = Material->GetMaterial())
			{
				if (IsTranslucentBlendMode(Base->BlendMode) && Base->TranslucencyLightingMode != TLM_SurfacePerPixelLighting)
				{
					Add(TEXT("ERROR"), Base->GetPathName(), TEXT("translucent toon without Surface ForwardShading: the toon shading never runs"), Base->GetPathName());
				}
			}
		}
	}
	for (const TPair<FString, TArray<FString>>& Pair : SdfTextures)
	{
		if (const UTexture* Texture = FindObject<UTexture>(nullptr, *Pair.Key))
		{
			const bool bBad = Texture->SRGB || Texture->CompressionSettings == TC_Default || Texture->CompressionSettings == TC_Normalmap;
			Add(bBad ? TEXT("WARN") : TEXT("INFO"), Pair.Key, FString::Printf(TEXT("SDF texture (%d instances): compression %s, sRGB %d -- the SDF lane is 16-bit, a BC / sRGB source caps it"),
				Pair.Value.Num(), *UEnum::GetValueAsString(Texture->CompressionSettings.GetValue()), Texture->SRGB ? 1 : 0), Pair.Key);
		}
	}
	Add(TEXT("INFO"), TEXT("level"), FString::Printf(TEXT("%s: %d actors with toon materials, %d toon materials checked"), *World->GetName(), ToonActors, Seen.Num()));

	// Ramp atlases whose rows are missing or point at the disabled /MooaToon/ plugin (white placeholders).
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UCurveLinearColorAtlas::StaticClass()->GetClassPathName());
	Filter.PackagePaths = { FName(TEXT("/Game")), FName(TEXT("/MoonToon")) };
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Atlases;
	Registry.GetAssets(Filter, Atlases);
	for (const FAssetData& Asset : Atlases)
	{
		const UCurveLinearColorAtlas* Atlas = Cast<UCurveLinearColorAtlas>(Asset.GetAsset());
		const FString Path = Asset.PackageName.ToString();
		if (!Atlas)
		{
			Add(TEXT("ERROR"), Path, TEXT("atlas failed to load"), Path);
			continue;
		}
		TArray<FString> Bad;
		for (int32 Row = 0; Row < Atlas->GradientCurves.Num(); ++Row)
		{
			const UCurveLinearColor* Curve = Atlas->GradientCurves[Row];
			if (!Curve)
			{
				Bad.Add(FString::Printf(TEXT("%d: None"), Row));
			}
			else if (Curve->GetPathName().StartsWith(TEXT("/MooaToon/")))
			{
				Bad.Add(FString::Printf(TEXT("%d: %s"), Row, *Curve->GetPathName()));
			}
		}
		TArray<FName> Referencers;
		Registry.GetReferencers(Asset.PackageName, Referencers);
		if (Bad.Num() > 0)
		{
			Add(Referencers.Num() > 0 ? TEXT("ERROR") : TEXT("INFO"), Path, FString::Printf(TEXT("%d / %d ramp rows missing or under /MooaToon/ (white placeholder)%s: %s"),
				Bad.Num(), Atlas->GradientCurves.Num(), Referencers.Num() > 0 ? TEXT("") : TEXT(", but nothing references this atlas"),
				*FString::Join(TArray<FString>(Bad.GetData(), FMath::Min(Bad.Num(), 6)), TEXT(", "))), Path);
		}
	}

	// The GPU self-check: every validate bit on screen, with a pixel showing it.
	if (bWithGpuChecks && IsAvailable())
	{
		const FMoonToonRegionStats Stats = RegionStatsSync(TEXT("validate"), 0, -1, true, FIntPoint::ZeroValue, FIntPoint::ZeroValue, 5.0f);
		if (!Stats.bValid)
		{
			Add(TEXT("INFO"), TEXT("gpu"), TEXT("GPU self-check skipped: ") + Stats.Error);
		}
		for (const FMoonToonRegionValue& Value : Stats.Values)
		{
			int32 Severity = 1;
			for (const MoonToonDebug::FValidateInfo& Info : MoonToonDebug::GetValidateInfos())
			{
				if ((int64)Info.Bit == Value.Key)
				{
					Severity = Info.Severity;
				}
			}
			Add(Severity >= 3 ? TEXT("ERROR") : (Severity == 2 ? TEXT("WARN") : TEXT("INFO")), TEXT("gpu"),
				FString::Printf(TEXT("%s on %d pixels (bounds %d,%d - %d,%d)"), *Value.Name, Value.PixelCount, Value.BoundsMin.X, Value.BoundsMin.Y, Value.BoundsMax.X, Value.BoundsMax.Y),
				FString(), Value.Sample);
		}
	}

	const auto Order = [](const FString& Severity) { return Severity == TEXT("ERROR") ? 0 : (Severity == TEXT("WARN") ? 1 : 2); };
	Out.StableSort([&Order](const FMoonToonValidationIssue& X, const FMoonToonValidationIssue& Y)
	{
		const int32 OX = Order(X.Severity);
		const int32 OY = Order(Y.Severity);
		return OX != OY ? OX < OY : X.Where < Y.Where;
	});
	return Out;
}
