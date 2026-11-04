// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonDebugLibrary.h"

#include "MoonToonDebugMcp.h"
#include "MoonToonDebugSubsystem.h"
#include "MoonToonToolsUserSettings.h"

#include "Dom/JsonObject.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString DebugLibSerialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString DebugLibError(const FString& Message)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("valid"), false);
		Root->SetStringField(TEXT("error"), Message);
		return DebugLibSerialize(Root);
	}

	UMoonToonDebugSubsystem* DebugLibSubsystem()
	{
		return UMoonToonDebugSubsystem::Get();
	}

	FString DebugLibSettingsJson(const MoonToonDebug::FViewSettings& Settings)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("valid"), true);
		Root->SetStringField(TEXT("view"), Settings.View);
		Root->SetNumberField(TEXT("channel"), Settings.Channel);
		Root->SetNumberField(TEXT("bit"), Settings.Bit);
		Root->SetBoolField(TEXT("resolved"), Settings.bResolved);
		Root->SetNumberField(TEXT("range_min"), Settings.RangeMin);
		Root->SetNumberField(TEXT("range_max"), Settings.RangeMax);
		Root->SetNumberField(TEXT("non_toon_mode"), Settings.NonToonMode);
		Root->SetNumberField(TEXT("isolate_key"), Settings.IsolateKey);
		Root->SetBoolField(TEXT("legend"), Settings.bLegend);
		Root->SetBoolField(TEXT("print"), Settings.bPrint);
		return DebugLibSerialize(Root);
	}
}

FString UMoonToonDebugLibrary::EditorStatusJson()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	FString WhyNot;
	const bool bAvailable = Sub && Sub->IsAvailable(&WhyNot);
	Root->SetBoolField(TEXT("valid"), true);
	Root->SetBoolField(TEXT("available"), bAvailable);
	Root->SetBoolField(TEXT("mcp_running"), MoonToonDebugMcp::IsRunning());
	Root->SetStringField(TEXT("mcp_url"), MoonToonDebugMcp::GetUrl());
	if (!bAvailable)
	{
		Root->SetStringField(TEXT("why_not"), Sub ? WhyNot : FString(TEXT("the debugger subsystem is not running")));
	}
	Root->SetNumberField(TEXT("api_version"), MoonToonDebug::GetApiVersion());
	if (Sub)
	{
		FLevelEditorViewportClient* Client = Sub->GetTargetViewportClient();
		if (Client && Client->Viewport)
		{
			const FIntPoint Size = Client->Viewport->GetSizeXY();
			Root->SetNumberField(TEXT("viewport_width"), Size.X);
			Root->SetNumberField(TEXT("viewport_height"), Size.Y);
			Root->SetBoolField(TEXT("viewport_realtime"), Client->IsRealtime());
		}
		if (UWorld* World = Sub->GetTargetWorld())
		{
			Root->SetStringField(TEXT("world"), World->GetPathName());
		}
		Root->SetStringField(TEXT("view"), Sub->GetCurrentViewName());
		Root->SetNumberField(TEXT("pending_requests"), MoonToonDebug::GetPendingRequestCount());
	}
	return DebugLibSerialize(Root);
}

FString UMoonToonDebugLibrary::ListDebugViewsJson()
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	if (!Sub)
	{
		return DebugLibError(TEXT("no debugger subsystem"));
	}
	TArray<TSharedPtr<FJsonValue>> Views;
	for (const FMoonToonDebugViewInfo& View : Sub->GetViews())
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("id"), View.Id);
		Entry->SetStringField(TEXT("name"), View.Name);
		Entry->SetStringField(TEXT("display_name"), View.DisplayName);
		Entry->SetStringField(TEXT("group"), View.Group);
		Entry->SetStringField(TEXT("encoding"), View.Encoding);
		TArray<TSharedPtr<FJsonValue>> Channels;
		for (const FString& Channel : View.ChannelNames)
		{
			Channels.Add(MakeShared<FJsonValueString>(Channel));
		}
		Entry->SetArrayField(TEXT("channels"), Channels);
		Entry->SetBoolField(TEXT("bit"), View.bHasBit);
		Entry->SetBoolField(TEXT("resolved"), View.bHasResolved);
		Views.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), true);
	Root->SetArrayField(TEXT("views"), Views);
	return DebugLibSerialize(Root);
}

FString UMoonToonDebugLibrary::SetDebugView(const FString& View, int32 Channel, int32 Bit, bool bResolved, float RangeMin, float RangeMax, int32 NonToonMode, int32 IsolateKey, bool bLegend)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	if (!Sub)
	{
		return DebugLibError(TEXT("no debugger subsystem"));
	}
	const FString Trimmed = View.TrimStartAndEnd();
	if (!Trimmed.IsEmpty() && !Trimmed.Equals(TEXT("off"), ESearchCase::IgnoreCase) && !MoonToonDebug::FindView(Trimmed))
	{
		return DebugLibError(FString::Printf(TEXT("unknown view '%s' (list_debug_views)"), *View));
	}
	MoonToonDebug::FViewSettings Settings = Sub->GetViewSettings();
	Settings.View = Trimmed;
	Settings.Channel = Channel;
	Settings.Bit = Bit;
	Settings.bResolved = bResolved;
	Settings.RangeMin = RangeMin;
	Settings.RangeMax = RangeMax;
	Settings.NonToonMode = NonToonMode;
	Settings.IsolateKey = IsolateKey;
	Settings.bLegend = bLegend;
	Sub->SetViewSettings(Settings);
	return DebugLibSettingsJson(Sub->GetViewSettings());
}

FString UMoonToonDebugLibrary::GetDebugViewJson()
{
	return DebugLibSettingsJson(MoonToonDebug::GetViewSettings());
}

FString UMoonToonDebugLibrary::GetPixelJson(float X, float Y, const FString& Space, bool bTrace)
{
	return GetPixel(X, Y, Space, bTrace).Json;
}

FMoonToonPixelReport UMoonToonDebugLibrary::GetPixel(float X, float Y, const FString& Space, bool bTrace)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	if (!Sub)
	{
		FMoonToonPixelReport Report;
		Report.Error = TEXT("no debugger subsystem");
		return Report;
	}
	return Sub->InspectSync(FVector2D(X, Y), Space, bTrace);
}

FString UMoonToonDebugLibrary::GetRegionStatsJson(const FString& View, int32 Channel, int32 Bit, bool bPerBit, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? Sub->RegionStatsSync(View, Channel, Bit, bPerBit, FIntPoint(MinX, MinY), FIntPoint(MaxX, MaxY)).Json : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::CaptureJson(const FString& View, int32 Channel, int32 Frames, const FString& OutputDirectory)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->CaptureSync(View, Channel, Frames, OutputDirectory)) : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::DiffCapturesJson(const FString& A, const FString& B, const FString& NoiseA, const FString& NoiseB, float Threshold, const FString& MaskPath)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	if (!Sub)
	{
		return DebugLibError(TEXT("no debugger subsystem"));
	}
	const FMoonToonDiffResult Result = Sub->DiffCaptures(A, B, NoiseA, NoiseB, Threshold, MaskPath);
	return Result.bValid ? Result.Json : DebugLibError(Result.Error);
}

FString UMoonToonDebugLibrary::ListCapturesJson(const FString& Directory)
{
	FString Dir = Directory;
	if (Dir.IsEmpty())
	{
		Dir = UMoonToonToolsUserSettings::Get().DebugCaptureDirectory;
		if (Dir.IsEmpty())
		{
			Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MoonToonDebug") / TEXT("captures"));
		}
	}
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
	Files.Sort([&Dir](const FString& X, const FString& Y)
	{
		return IFileManager::Get().GetTimeStamp(*(Dir / X)) > IFileManager::Get().GetTimeStamp(*(Dir / Y));
	});
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FString& File : Files)
	{
		Entries.Add(MakeShared<FJsonValueString>(Dir / File));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), true);
	Root->SetStringField(TEXT("directory"), Dir);
	Root->SetArrayField(TEXT("captures"), Entries);
	return DebugLibSerialize(Root);
}

FString UMoonToonDebugLibrary::ListToonActorsJson(bool bWithGpuData)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->ListToonActors(bWithGpuData)) : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::DumpConstantTableJson()
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->DumpConstantTable()) : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::DumpRampAtlasJson()
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->DumpRampAtlas()) : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::DumpProfilesJson()
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->DumpProfiles()) : DebugLibError(TEXT("no debugger subsystem"));
}

FString UMoonToonDebugLibrary::DumpStyleJson()
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	if (!Sub)
	{
		return DebugLibError(TEXT("no debugger subsystem"));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), true);
	TSharedPtr<FJsonObject> Layers;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UMoonToonDebugSubsystem::ToJson(Sub->GetStyleLayers()));
	if (FJsonSerializer::Deserialize(Reader, Layers) && Layers.IsValid())
	{
		Root->SetArrayField(TEXT("layers"), Layers->GetArrayField(TEXT("layers")));
	}
	// The style fields as the last inspected pixel resolved them (its profile overrides included).
	const FMoonToonPixelReport& Report = Sub->GetReport();
	if (Report.bValid)
	{
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		for (const FMoonToonDebugField& Field : Report.Fields)
		{
			if (Field.Section == TEXT("style"))
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("value"), Field.Value);
				Entry->SetStringField(TEXT("source"), Field.Source);
				Fields->SetObjectField(Field.Name, Entry);
			}
		}
		Root->SetObjectField(TEXT("last_pixel_style"), Fields);
		Root->SetStringField(TEXT("last_pixel"), FString::Printf(TEXT("%d %d"), Report.ViewportPixel.X, Report.ViewportPixel.Y));
	}
	return DebugLibSerialize(Root);
}

FString UMoonToonDebugLibrary::ValidateJson(bool bWithGpuChecks)
{
	UMoonToonDebugSubsystem* Sub = DebugLibSubsystem();
	return Sub ? UMoonToonDebugSubsystem::ToJson(Sub->Validate(bWithGpuChecks)) : DebugLibError(TEXT("no debugger subsystem"));
}

void UMoonToonDebugLibrary::OpenDebuggerPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("MoonToonDebugger")));
}

FString UMoonToonDebugLibrary::StartMcpServer()
{
	FString Error;
	if (!MoonToonDebugMcp::Start(&Error))
	{
		return DebugLibError(Error);
	}
	return MoonToonDebugMcp::GetStatusJson();
}

FString UMoonToonDebugLibrary::StopMcpServer()
{
	MoonToonDebugMcp::Stop();
	return MoonToonDebugMcp::GetStatusJson();
}

FString UMoonToonDebugLibrary::McpStatusJson()
{
	return MoonToonDebugMcp::GetStatusJson();
}
