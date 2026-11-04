// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonDebugMcp.h"

#include "MoonToonDebug.h"
#include "MoonToonDebugLibrary.h"
#include "MoonToonToolsUserSettings.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpPath.h"
#include "HttpRouteHandle.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonMcp, Log, All);

namespace MoonToonDebugMcpPrivate
{
	const TCHAR* const ServerName = TEXT("moontoon-debug");
	const TCHAR* const ServerVersion = TEXT("1.0.0");
	const TCHAR* const RoutePath = TEXT("/mcp");
	/**
	 * Newest first; an unknown requested version (e.g. a newer one) is answered with the first, and the client
	 * falls back to it. All of these share the transport used here: POST -> one JSON response, GET -> 405.
	 */
	const TCHAR* const ProtocolVersions[] = { TEXT("2025-11-25"), TEXT("2025-06-18"), TEXT("2025-03-26") };

	struct FPending
	{
		FString Body;
		FHttpResultCallback OnComplete;
	};

	struct FTool
	{
		FString Name;
		FString Description;
		TSharedPtr<FJsonObject> InputSchema;
		TFunction<FString(const TSharedPtr<FJsonObject>&)> Invoke;
	};

	struct FServer
	{
		TSharedPtr<IHttpRouter> Router;
		FHttpRouteHandle RouteHandle;
		int32 Port = 0;
		FString Token;
		FString SessionId;
		TArray<FPending> Queue;
		FTSTicker::FDelegateHandle TickHandle;
		TArray<FTool> Tools;
		int64 RequestCount = 0;
		int64 RejectedCount = 0;
		FString LastError;
		FString LastClient;
	};

	FServer& Server()
	{
		static FServer Instance;
		return Instance;
	}

	// --- JSON helpers -----------------------------------------------------------------------------------

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	FString ArgString(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, const FString& Default = FString())
	{
		FString Value;
		return Args.IsValid() && Args->TryGetStringField(Name, Value) ? Value : Default;
	}

	double ArgNumber(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, double Default)
	{
		double Value = 0.0;
		return Args.IsValid() && Args->TryGetNumberField(Name, Value) ? Value : Default;
	}

	int32 ArgInt(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, int32 Default)
	{
		return (int32)FMath::RoundToInt(ArgNumber(Args, Name, (double)Default));
	}

	bool ArgBool(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, bool bDefault)
	{
		bool bValue = false;
		return Args.IsValid() && Args->TryGetBoolField(Name, bValue) ? bValue : bDefault;
	}

	FString ErrorJson(const FString& Message)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("valid"), false);
		Root->SetStringField(TEXT("error"), Message);
		return Serialize(Root);
	}

	// --- token ------------------------------------------------------------------------------------------

	FString TokenPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MoonToonDebug") / TEXT("mcp_token.txt"));
	}

	FString NewToken()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	FString LoadOrCreateToken()
	{
		FString Token;
		if (FFileHelper::LoadFileToString(Token, *TokenPath()))
		{
			Token.TrimStartAndEndInline();
			if (Token.Len() >= 32)
			{
				return Token;
			}
		}
		Token = NewToken();
		FFileHelper::SaveStringToFile(Token, *TokenPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Token;
	}

	// --- tools ------------------------------------------------------------------------------------------

	TSharedPtr<FJsonObject> Schema(const ANSICHAR* Json)
	{
		TSharedPtr<FJsonObject> Object = ParseObject(FString(UTF8_TO_TCHAR(Json)));
		if (!Object.IsValid())
		{
			UE_LOG(LogMoonToonMcp, Error, TEXT("Bad tool schema: %s"), UTF8_TO_TCHAR(Json));
			Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("object"));
		}
		return Object;
	}

	FString ReadCaptureMeta(const FString& Path)
	{
		// Only the debugger's own files: the capture directory (setting) and Saved/MoonToonDebug.
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Full);
		FPaths::CollapseRelativeDirectories(Full);
		TArray<FString> Roots;
		Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MoonToonDebug")));
		const FString Custom = UMoonToonToolsUserSettings::Get().DebugCaptureDirectory;
		if (!Custom.IsEmpty())
		{
			Roots.Add(FPaths::ConvertRelativePathToFull(Custom));
		}
		bool bAllowed = false;
		for (FString Root : Roots)
		{
			FPaths::NormalizeDirectoryName(Root);
			FPaths::CollapseRelativeDirectories(Root);
			bAllowed |= Full.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
		}
		if (!bAllowed || !Full.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
		{
			return ErrorJson(TEXT("read_capture_meta only reads the .json files of the capture directory (list_captures)"));
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Full))
		{
			return ErrorJson(FString::Printf(TEXT("cannot read %s"), *Full));
		}
		return Text;
	}

	void AddTool(const TCHAR* Name, const TCHAR* Description, const ANSICHAR* InputSchema, TFunction<FString(const TSharedPtr<FJsonObject>&)> Invoke)
	{
		FTool& Tool = Server().Tools.AddDefaulted_GetRef();
		Tool.Name = Name;
		Tool.Description = Description;
		Tool.InputSchema = Schema(InputSchema);
		Tool.Invoke = MoveTemp(Invoke);
	}

	void BuildTools()
	{
		if (Server().Tools.Num() > 0)
		{
			return;
		}
		const ANSICHAR* const NoArgs = R"json({"type":"object","properties":{}})json";

		AddTool(TEXT("editor_status"),
			TEXT("Is the MoonToon debugger usable (Substrate, debug shaders compiled with r.MoonToon.Debug.Shaders=1, engine/plugin API version), the target level viewport and world, the current debug view, the MCP server. Call this first."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::EditorStatusJson(); });

		AddTool(TEXT("list_debug_views"),
			TEXT("Every MoonToon debug view (record fields, self-checks, intermediate buffers, shading terms): name, group, encoding, channel names, whether it takes a bit / resolved flag."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::ListDebugViewsJson(); });

		AddTool(TEXT("set_debug_view"),
			TEXT("Shows a debug view full screen in the editor viewports (replaces the image after tonemapping and upscaling; the same state as the r.MoonToon.Debug.* cvars and the debugger panel). view \"off\" turns it off. Returns the settings in effect."),
			R"json({"type":"object","properties":{
				"view":{"type":"string","description":"a name from list_debug_views, or \"off\""},
				"channel":{"type":"integer","description":"channel index for multi-channel views","default":0},
				"bit":{"type":"integer","description":"-1 = all bits; 0..31 = only this bit (bit views)","default":-1},
				"resolved":{"type":"boolean","description":"true = the value shading uses (after constant table / profile), false = raw record bits","default":true},
				"range_min":{"type":"number","default":0},
				"range_max":{"type":"number","description":"range_min == range_max = the view's default range","default":0},
				"non_toon":{"type":"integer","description":"non-toon pixels: 0 dim, 1 black, 2 as is","default":0},
				"isolate":{"type":"integer","description":"-1 = all values; otherwise only pixels whose key equals this value are painted","default":-1},
				"legend":{"type":"boolean","default":true}},
				"required":["view"]})json",
			[](const TSharedPtr<FJsonObject>& A)
			{
				return UMoonToonDebugLibrary::SetDebugView(ArgString(A, TEXT("view")), ArgInt(A, TEXT("channel"), 0), ArgInt(A, TEXT("bit"), -1),
					ArgBool(A, TEXT("resolved"), true), (float)ArgNumber(A, TEXT("range_min"), 0.0), (float)ArgNumber(A, TEXT("range_max"), 0.0),
					ArgInt(A, TEXT("non_toon"), 0), ArgInt(A, TEXT("isolate"), -1), ArgBool(A, TEXT("legend"), true));
			});

		AddTool(TEXT("get_debug_view"),
			TEXT("The debug view settings in effect."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::GetDebugViewJson(); });

		AddTool(TEXT("get_pixel"),
			TEXT("Everything about one pixel of the target viewport: the decoded MoonToon record with each value's source (material / profile / style preset @ volume / default / constant-table row), the intermediate buffers, the measured per-stage scene colour ledger, and the re-evaluated main light (trace). Synchronous: draws the viewport once and waits for the GPU (a minimised editor renders nothing and returns an error)."),
			R"json({"type":"object","properties":{
				"x":{"type":"number"},"y":{"type":"number"},
				"space":{"type":"string","enum":["viewport","render","uv"],"description":"viewport = the viewport's pixels (what a screenshot shows); render = the internal view rect, 0-based; uv = 0..1","default":"viewport"},
				"trace":{"type":"boolean","description":"also re-evaluate the main light","default":true}},
				"required":["x","y"]})json",
			[](const TSharedPtr<FJsonObject>& A)
			{
				return UMoonToonDebugLibrary::GetPixelJson((float)ArgNumber(A, TEXT("x"), 0.0), (float)ArgNumber(A, TEXT("y"), 0.0),
					ArgString(A, TEXT("space"), TEXT("viewport")), ArgBool(A, TEXT("trace"), true));
			});

		AddTool(TEXT("get_region_stats"),
			TEXT("For every value of a debug view (need not be on screen): pixel count, bounds and a sample pixel in render and viewport space -- e.g. 'where is actor 2's face' or 'which profile ids are on screen'. Rect in render pixels, 0-based; max <= min = the whole view. Synchronous."),
			R"json({"type":"object","properties":{
				"view":{"type":"string"},"channel":{"type":"integer","default":0},"bit":{"type":"integer","default":-1},
				"per_bit":{"type":"boolean","description":"count every set bit separately (bit views)","default":false},
				"min_x":{"type":"integer","default":0},"min_y":{"type":"integer","default":0},
				"max_x":{"type":"integer","default":0},"max_y":{"type":"integer","default":0}},
				"required":["view"]})json",
			[](const TSharedPtr<FJsonObject>& A)
			{
				return UMoonToonDebugLibrary::GetRegionStatsJson(ArgString(A, TEXT("view")), ArgInt(A, TEXT("channel"), 0), ArgInt(A, TEXT("bit"), -1),
					ArgBool(A, TEXT("per_bit"), false), ArgInt(A, TEXT("min_x"), 0), ArgInt(A, TEXT("min_y"), 0), ArgInt(A, TEXT("max_x"), 0), ArgInt(A, TEXT("max_y"), 0));
			});

		AddTool(TEXT("capture"),
			TEXT("Reads a view back at the render resolution and writes <time>_<view>.exr (raw values, linear float), _id.png (16-bit keys, exact ids), _color.png (false colour) and .json (coordinates, cvars, camera, frame). view = a debug view, or scene.hdr (linear, before tonemapping) / scene.final. frames = frames drawn first (TSR history, streaming). Returns the file paths."),
			R"json({"type":"object","properties":{
				"view":{"type":"string"},"channel":{"type":"integer","default":0},
				"frames":{"type":"integer","default":1},
				"output_directory":{"type":"string","description":"default: the capture directory"}},
				"required":["view"]})json",
			[](const TSharedPtr<FJsonObject>& A)
			{
				return UMoonToonDebugLibrary::CaptureJson(ArgString(A, TEXT("view")), ArgInt(A, TEXT("channel"), 0), ArgInt(A, TEXT("frames"), 1), ArgString(A, TEXT("output_directory")));
			});

		AddTool(TEXT("list_captures"),
			TEXT("The captures' .json meta files in a directory (default: the capture directory), newest first."),
			R"json({"type":"object","properties":{"directory":{"type":"string"}}})json",
			[](const TSharedPtr<FJsonObject>& A) { return UMoonToonDebugLibrary::ListCapturesJson(ArgString(A, TEXT("directory"))); });

		AddTool(TEXT("read_capture_meta"),
			TEXT("The .json meta file of a capture (coordinate mapping, view settings, cvars, camera, frame). Only files under the capture directory."),
			R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})json",
			[](const TSharedPtr<FJsonObject>& A) { return ReadCaptureMeta(ArgString(A, TEXT("path"))); });

		AddTool(TEXT("diff_captures"),
			TEXT("Compares two captures' raw EXRs (a, b). With noise_a / noise_b (two captures of the SAME settings) it reports the change beyond the noise floor and a SAME/DIFF verdict (stable pixels <= 1.5 x noise + 50). threshold in raw units (default 0.01). Optionally writes a difference mask PNG."),
			R"json({"type":"object","properties":{
				"a":{"type":"string"},"b":{"type":"string"},
				"noise_a":{"type":"string"},"noise_b":{"type":"string"},
				"threshold":{"type":"number","default":0},
				"mask_path":{"type":"string"}},
				"required":["a","b"]})json",
			[](const TSharedPtr<FJsonObject>& A)
			{
				return UMoonToonDebugLibrary::DiffCapturesJson(ArgString(A, TEXT("a")), ArgString(A, TEXT("b")), ArgString(A, TEXT("noise_a")), ArgString(A, TEXT("noise_b")),
					(float)ArgNumber(A, TEXT("threshold"), 0.0), ArgString(A, TEXT("mask_path")));
			});

		AddTool(TEXT("list_toon_actors"),
			TEXT("Every actor with toon materials or a MoonToonActorComponent in the target world: actor id, label, path, component present, toon materials with their profile, the 13 actor-table rows as the GPU holds them (named), on-screen pixel count, warnings."),
			R"json({"type":"object","properties":{"with_gpu_data":{"type":"boolean","description":"read back the actor table and pixel counts (draws one frame)","default":true}}})json",
			[](const TSharedPtr<FJsonObject>& A) { return UMoonToonDebugLibrary::ListToonActorsJson(ArgBool(A, TEXT("with_gpu_data"), true)); });

		AddTool(TEXT("dump_constant_table"),
			TEXT("The MoonToon feature constant table (T37): capacity, rows, each row's 11 channel values, per-pixel mask and the materials using it."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::DumpConstantTableJson(); });

		AddTool(TEXT("dump_profiles"),
			TEXT("MoonToon profile ids -> assets, with their settings by name."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::DumpProfilesJson(); });

		AddTool(TEXT("dump_ramp_atlas"),
			TEXT("The dynamic ramp atlas: slices (diffuse / specular) -> source curve or texture, row, segment layout, last use."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::DumpRampAtlasJson(); });

		AddTool(TEXT("dump_style"),
			TEXT("The post process volumes' MoonToon style presets around the camera (priority order, overridden fields), plus the last inspected pixel's resolved style fields with sources."),
			NoArgs,
			[](const TSharedPtr<FJsonObject>&) { return UMoonToonDebugLibrary::DumpStyleJson(); });

		AddTool(TEXT("validate"),
			TEXT("The MoonToon content rules (T44: GI intensity, toon actors without a MoonToonActorComponent, material instance parameters, SDF texture formats, translucent lighting mode, ramp atlas rows pointing at disabled curves) plus, with gpu_checks, the GPU self-check counts per bit (record truncated, reserved bits, bad format / table row, ...)."),
			R"json({"type":"object","properties":{"gpu_checks":{"type":"boolean","default":true}}})json",
			[](const TSharedPtr<FJsonObject>& A) { return UMoonToonDebugLibrary::ValidateJson(ArgBool(A, TEXT("gpu_checks"), true)); });
	}

	// --- JSON-RPC ---------------------------------------------------------------------------------------

	TSharedPtr<FJsonValue> IdOrNull(const TSharedPtr<FJsonValue>& Id)
	{
		if (Id.IsValid())
		{
			return Id;
		}
		return MakeShared<FJsonValueNull>();
	}

	TSharedRef<FJsonObject> RpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetField(TEXT("id"), IdOrNull(Id));
		Root->SetObjectField(TEXT("result"), Result);
		return Root;
	}

	TSharedRef<FJsonObject> RpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetField(TEXT("id"), IdOrNull(Id));
		Root->SetObjectField(TEXT("error"), Error);
		return Root;
	}

	TSharedRef<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params)
	{
		FString Requested;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), Requested);
		}
		FString Version = ProtocolVersions[0];
		for (const TCHAR* Supported : ProtocolVersions)
		{
			if (Requested == Supported)
			{
				Version = Supported;
			}
		}
		TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
		Tools->SetBoolField(TEXT("listChanged"), false);
		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), Tools);
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), ServerName);
		Info->SetStringField(TEXT("version"), ServerVersion);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Version);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), Info);
		Result->SetStringField(TEXT("instructions"),
			TEXT("MoonToon (toon shading on Substrate) debugger inside the Unreal editor. Start with editor_status. ")
			TEXT("Pixel coordinates default to the level viewport's own pixels (what a viewport screenshot shows). ")
			TEXT("get_pixel / get_region_stats / capture / list_toon_actors / validate draw the viewport and wait for the GPU, so the editor window must not be minimised. ")
			TEXT("set_debug_view changes what the user sees in the viewport; set it back to \"off\" when done."));
		return Result;
	}

	TSharedRef<FJsonObject> HandleToolsList()
	{
		BuildTools();
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FTool& Tool : Server().Tools)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Tool.Name);
			Entry->SetStringField(TEXT("description"), Tool.Description);
			Entry->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
			List.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), List);
		return Result;
	}

	/** Null when the tool does not exist (a JSON-RPC error); otherwise a CallToolResult. */
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params)
	{
		BuildTools();
		FString Name;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name))
		{
			return nullptr;
		}
		const FTool* Tool = Server().Tools.FindByPredicate([&Name](const FTool& Candidate) { return Candidate.Name == Name; });
		if (!Tool)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr && ArgsPtr->IsValid())
		{
			Args = *ArgsPtr;
		}

		const FString Text = Tool->Invoke(Args);

		// A {"valid": false, ...} answer is a tool error the model should see as one. Objects go out condensed.
		bool bIsError = false;
		FString Condensed = Text;
		if (const TSharedPtr<FJsonObject> Parsed = ParseObject(Text))
		{
			bool bValid = true;
			if (Parsed->TryGetBoolField(TEXT("valid"), bValid))
			{
				bIsError = !bValid;
			}
			Condensed = Serialize(Parsed.ToSharedRef());
		}
		TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("type"), TEXT("text"));
		Content->SetStringField(TEXT("text"), Condensed);
		TArray<TSharedPtr<FJsonValue>> ContentList;
		ContentList.Add(MakeShared<FJsonValueObject>(Content));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("content"), ContentList);
		Result->SetBoolField(TEXT("isError"), bIsError);
		return Result;
	}

	TUniquePtr<FHttpServerResponse> JsonResponse(const FString& Json, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Json, TEXT("application/json"));
		Response->Code = Code;
		if (!Server().SessionId.IsEmpty())
		{
			TArray<FString> Values;
			Values.Add(Server().SessionId);
			Response->Headers.Add(FString(TEXT("Mcp-Session-Id")), MoveTemp(Values));
		}
		return Response;
	}

	/** One JSON-RPC message -> the HTTP response (202 for notifications and responses). */
	TUniquePtr<FHttpServerResponse> Process(const FString& Body)
	{
		const TSharedPtr<FJsonObject> Message = ParseObject(Body);
		if (!Message.IsValid())
		{
			return JsonResponse(Serialize(RpcError(nullptr, -32700, TEXT("Parse error (batches are not supported)"))), EHttpServerResponseCodes::BadRequest);
		}
		FString Method;
		const bool bHasMethod = Message->TryGetStringField(TEXT("method"), Method);
		const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id"));
		if (!bHasMethod || !Id.IsValid())
		{
			// A notification (notifications/initialized, notifications/cancelled ...) or a response: nothing to answer.
			TUniquePtr<FHttpServerResponse> Accepted = FHttpServerResponse::Create(FString(), TEXT("application/json"));
			Accepted->Code = EHttpServerResponseCodes::Accepted;
			return Accepted;
		}
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> Params;
		if (Message->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
		{
			Params = *ParamsPtr;
		}

		if (Method == TEXT("initialize"))
		{
			Server().SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			return JsonResponse(Serialize(RpcResult(Id, HandleInitialize(Params))));
		}
		if (Method == TEXT("ping"))
		{
			return JsonResponse(Serialize(RpcResult(Id, MakeShared<FJsonObject>())));
		}
		if (Method == TEXT("tools/list"))
		{
			return JsonResponse(Serialize(RpcResult(Id, HandleToolsList())));
		}
		if (Method == TEXT("tools/call"))
		{
			const TSharedPtr<FJsonObject> Result = HandleToolsCall(Params);
			return JsonResponse(Serialize(Result.IsValid()
				? RpcResult(Id, Result.ToSharedRef())
				: RpcError(Id, -32602, TEXT("Unknown tool or missing name (tools/list)"))));
		}
		return JsonResponse(Serialize(RpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method))));
	}

	// --- HTTP -------------------------------------------------------------------------------------------

	const FString* FirstHeader(const FHttpServerRequest& Request, const TCHAR* Name)
	{
		// TMap<FString, ...> compares keys case-insensitively, as HTTP header names are.
		const TArray<FString>* Values = Request.Headers.Find(Name);
		return Values && Values->Num() > 0 ? &(*Values)[0] : nullptr;
	}

	bool IsLoopbackPeer(const FHttpServerRequest& Request)
	{
		if (!Request.PeerAddress.IsValid())
		{
			return false;
		}
		const FString Address = Request.PeerAddress->ToString(false);
		return Address.StartsWith(TEXT("127.")) || Address == TEXT("::1") || Address.StartsWith(TEXT("::ffff:127."));
	}

	bool IsLocalOrigin(const FString& Origin)
	{
		return Origin.StartsWith(TEXT("http://127.0.0.1")) || Origin.StartsWith(TEXT("http://localhost")) || Origin.StartsWith(TEXT("http://[::1]"));
	}

	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		FServer& S = Server();
		const auto Refuse = [&S, &OnComplete](EHttpServerResponseCodes Code, const FString& Why)
		{
			++S.RejectedCount;
			S.LastError = Why;
			OnComplete(FHttpServerResponse::Error(Code, TEXT("moontoon_mcp"), Why));
		};

		if (!IsLoopbackPeer(Request))
		{
			Refuse(EHttpServerResponseCodes::Forbidden, TEXT("only loopback clients are served"));
			return true;
		}
		if (const FString* Origin = FirstHeader(Request, TEXT("Origin")))
		{
			if (!IsLocalOrigin(*Origin))
			{
				Refuse(EHttpServerResponseCodes::Forbidden, FString::Printf(TEXT("origin %s refused"), **Origin));
				return true;
			}
		}
		const FString* Authorization = FirstHeader(Request, TEXT("Authorization"));
		// 403 rather than 401: a 401 would send MCP clients looking for an OAuth server.
		if (!Authorization || *Authorization != FString(TEXT("Bearer ")) + S.Token)
		{
			Refuse(EHttpServerResponseCodes::Forbidden, FString::Printf(TEXT("missing or wrong bearer token (see %s)"), *TokenPath()));
			return true;
		}
		if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
		{
			// No server-initiated stream.
			OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::BadMethod, TEXT("no_sse"), TEXT("this server does not offer an SSE stream")));
			return true;
		}
		if (Request.Verb == EHttpServerRequestVerbs::VERB_DELETE)
		{
			S.SessionId.Reset();
			OnComplete(FHttpServerResponse::Ok());
			return true;
		}

		// The body is UTF-8. Run it on the next tick, one request at a time (tools may draw and wait for the GPU).
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		FPending& Pending = S.Queue.AddDefaulted_GetRef();
		Pending.Body = FString(Converted.Length(), Converted.Get());
		Pending.OnComplete = OnComplete;
		S.LastClient = Request.PeerAddress->ToString(true);
		return true;
	}

	bool Tick(float DeltaTime)
	{
		FServer& S = Server();
		if (S.Queue.Num() == 0)
		{
			return true;
		}
		FPending Pending = MoveTemp(S.Queue[0]);
		S.Queue.RemoveAt(0);
		++S.RequestCount;
		Pending.OnComplete(Process(Pending.Body));
		return true;
	}
}

// No file-scope using-directive: under unity builds it would leak into the module's other .cpp files.
bool MoonToonDebugMcp::Start(FString* OutError)
{
	using namespace MoonToonDebugMcpPrivate;
	FServer& S = Server();
	if (S.RouteHandle)
	{
		return true;
	}
	const int32 Port = UMoonToonToolsUserSettings::Get().McpPort;
	const auto Fail = [&S, OutError](const FString& Why)
	{
		S.LastError = Why;
		if (OutError)
		{
			*OutError = Why;
		}
		UE_LOG(LogMoonToonMcp, Warning, TEXT("%s"), *Why);
		return false;
	};
	if (Port <= 0 || Port > 65535)
	{
		return Fail(FString::Printf(TEXT("bad MCP port %d (Editor Preferences > MoonToon Debugger)"), Port));
	}

	S.Router = FHttpServerModule::Get().GetHttpRouter((uint32)Port, /*bFailOnBindFailure*/ true);
	if (!S.Router.IsValid())
	{
		return Fail(FString::Printf(TEXT("cannot listen on 127.0.0.1:%d (in use?) -- pick another Mcp Port and update the Claude Code entry"), Port));
	}
	S.RouteHandle = S.Router->BindRoute(FHttpPath(RoutePath),
		EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateStatic(&HandleRequest));
	if (!S.RouteHandle)
	{
		S.Router.Reset();
		return Fail(FString::Printf(TEXT("cannot bind %s on port %d (another server owns the route?)"), RoutePath, Port));
	}
	FHttpServerModule::Get().StartAllListeners();

	S.Port = Port;
	S.Token = LoadOrCreateToken();
	S.SessionId.Reset();
	S.LastError.Reset();
	if (!S.TickHandle.IsValid())
	{
		S.TickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("MoonToonDebugMcp"), 0.0f, &MoonToonDebugMcpPrivate::Tick);
	}
	BuildTools();
	UE_LOG(LogMoonToonMcp, Display, TEXT("MoonToon MCP server listening on %s (token: %s)"), *GetUrl(), *TokenPath());
	return true;
}

void MoonToonDebugMcp::Stop()
{
	using namespace MoonToonDebugMcpPrivate;
	FServer& S = Server();
	if (S.Router.IsValid() && S.RouteHandle && FHttpServerModule::IsAvailable())
	{
		S.Router->UnbindRoute(S.RouteHandle);
	}
	S.RouteHandle.Reset();
	S.Router.Reset();
	if (S.TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(S.TickHandle);
		S.TickHandle.Reset();
	}
	// Answer whatever was still queued, so no client waits forever.
	for (FPending& Pending : S.Queue)
	{
		Pending.OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("moontoon_mcp"), TEXT("server stopped")));
	}
	S.Queue.Reset();
	if (S.Port != 0)
	{
		UE_LOG(LogMoonToonMcp, Display, TEXT("MoonToon MCP server stopped (port %d stays bound by the engine's HTTP listener until the editor exits)"), S.Port);
	}
	S.Port = 0;
}

bool MoonToonDebugMcp::IsRunning()
{
	using namespace MoonToonDebugMcpPrivate;
	return Server().RouteHandle.IsValid();
}

int32 MoonToonDebugMcp::GetPort()
{
	using namespace MoonToonDebugMcpPrivate;
	return IsRunning() ? Server().Port : UMoonToonToolsUserSettings::Get().McpPort;
}

FString MoonToonDebugMcp::GetUrl()
{
	using namespace MoonToonDebugMcpPrivate;
	return FString::Printf(TEXT("http://127.0.0.1:%d%s"), GetPort(), RoutePath);
}

FString MoonToonDebugMcp::GetTokenPath()
{
	using namespace MoonToonDebugMcpPrivate;
	return TokenPath();
}

void MoonToonDebugMcp::RegenerateToken()
{
	using namespace MoonToonDebugMcpPrivate;
	const FString Token = NewToken();
	FFileHelper::SaveStringToFile(Token, *TokenPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Server().Token = Token;
	UE_LOG(LogMoonToonMcp, Display, TEXT("New MoonToon MCP token written to %s"), *TokenPath());
}

FString MoonToonDebugMcp::GetStatusJson()
{
	using namespace MoonToonDebugMcpPrivate;
	const FServer& S = Server();
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), true);
	Root->SetBoolField(TEXT("running"), IsRunning());
	Root->SetStringField(TEXT("url"), GetUrl());
	Root->SetStringField(TEXT("token_path"), TokenPath());
	Root->SetBoolField(TEXT("token_exists"), IFileManager::Get().FileExists(*TokenPath()));
	Root->SetNumberField(TEXT("requests"), (double)S.RequestCount);
	Root->SetNumberField(TEXT("rejected"), (double)S.RejectedCount);
	Root->SetNumberField(TEXT("queued"), S.Queue.Num());
	Root->SetStringField(TEXT("last_client"), S.LastClient);
	Root->SetStringField(TEXT("last_error"), S.LastError);
	return MoonToonDebugMcpPrivate::Serialize(Root);
}

// --- console --------------------------------------------------------------------------------------------

static FAutoConsoleCommand GMoonToonMcpStart(
	TEXT("MoonToon.Debug.Mcp.Start"),
	TEXT("Starts the MoonToon debugger's MCP server (http://127.0.0.1:<Mcp Port>/mcp, bearer token in Saved/MoonToonDebug/mcp_token.txt)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FString Error;
		if (!MoonToonDebugMcp::Start(&Error))
		{
			UE_LOG(LogMoonToonMcp, Error, TEXT("%s"), *Error);
		}
	}));

static FAutoConsoleCommand GMoonToonMcpStop(
	TEXT("MoonToon.Debug.Mcp.Stop"),
	TEXT("Stops the MoonToon debugger's MCP server."),
	FConsoleCommandDelegate::CreateLambda([]() { MoonToonDebugMcp::Stop(); }));

static FAutoConsoleCommand GMoonToonMcpStatus(
	TEXT("MoonToon.Debug.Mcp.Status"),
	TEXT("Prints the MoonToon debugger's MCP server state."),
	FConsoleCommandDelegate::CreateLambda([]() { UE_LOG(LogMoonToonMcp, Display, TEXT("%s"), *MoonToonDebugMcp::GetStatusJson()); }));

static FAutoConsoleCommand GMoonToonMcpNewToken(
	TEXT("MoonToon.Debug.Mcp.NewToken"),
	TEXT("Writes a new MoonToon MCP bearer token (Claude Code must read it again)."),
	FConsoleCommandDelegate::CreateLambda([]() { MoonToonDebugMcp::RegenerateToken(); }));
