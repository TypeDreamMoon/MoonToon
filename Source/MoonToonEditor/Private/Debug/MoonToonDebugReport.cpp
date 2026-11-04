// Copyright Dream Moon. All Rights Reserved.

// UMoonToonDebugSubsystem: the inspector report (raw renderer result -> named fields with their sources),
// region statistics, value names, and JSON. Every name list is expanded from the engine's shared headers
// (Engine/Shaders/Shared) -- kinds, terms, feature ids, slot names -- so a new feature shows up here without
// an edit. The component labels and the style-slot map below are the only hand-kept tables.

#include "MoonToonDebugSubsystem.h"
#include "MoonToonDebugNames.h"

#include "MoonToonDebugDefinitions.h"
#include "MoonToonFeatureRegistry.h"
#include "MoonToonRecordDefinitions.h"
#include "MoonToonStyleDefinitions.h"
#include "MoonToonProfileDefinitions.h"
#include "MoonToonActorDefinitions.h"

#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Paths.h"
#include "Engine/MoonToonStylePreset.h"
#include "UObject/UnrealType.h"

/** A style field's code default: the property of that name (or bName / bName-without-Enable) in the style preset's CDO. */
static bool MoonToonStyleCodeDefault(const FString& FieldName, double& OutValue)
{
	const UMoonToonStylePreset* Defaults = GetDefault<UMoonToonStylePreset>();
	if (!Defaults)
	{
		return false;
	}
	TArray<FString> Candidates;
	Candidates.Add(FieldName);
	Candidates.Add(TEXT("b") + FieldName);
	if (FieldName.EndsWith(TEXT("Enable")))
	{
		const FString Base = FieldName.LeftChop(6);
		Candidates.Add(Base);
		Candidates.Add(TEXT("b") + Base);
	}
	TFunction<bool(const UStruct*, const void*, int32)> Search = [&](const UStruct* Struct, const void* Data, int32 Depth) -> bool
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Data);
			const FString PropertyName = Property->GetName();
			if (Candidates.ContainsByPredicate([&PropertyName](const FString& Candidate) { return PropertyName.Equals(Candidate, ESearchCase::IgnoreCase); }))
			{
				if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
				{
					OutValue = Bool->GetPropertyValue(ValuePtr) ? 1.0 : 0.0;
					return true;
				}
				if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
				{
					OutValue = (double)Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
					return true;
				}
				if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
				{
					OutValue = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(ValuePtr) : (double)Numeric->GetSignedIntPropertyValue(ValuePtr);
					return true;
				}
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (Depth < 3 && Search(StructProperty->Struct, ValuePtr, Depth + 1))
				{
					return true;
				}
			}
		}
		return false;
	};
	return Search(Defaults->GetClass(), Defaults, 0);
}

namespace MoonToonDebugNames
{
	static float AsFloat(uint32 Word)
	{
		float Value;
		FMemory::Memcpy(&Value, &Word, sizeof(Value));
		return Value;
	}

	static FString KindName(uint32 Kind)
	{
#define MOONTOON_DEBUG_KIND_NAME(KindName, Module) if (Kind == MOON_TOON_KIND_##KindName) { return TEXT(#KindName); }
		MOON_TOON_KIND_LIST(MOONTOON_DEBUG_KIND_NAME)
#undef MOONTOON_DEBUG_KIND_NAME
		return FString::Printf(TEXT("kind %u"), Kind);
	}

	static FString TermNames(uint32 Terms)
	{
		TArray<FString> Names;
#define MOONTOON_DEBUG_TERM_NAME(TermName, Module, Bit, Hooks) if ((Terms & (1u << (Bit))) != 0u) { Names.Add(TEXT(#TermName)); }
		MOON_TOON_SHADING_TERM_LIST(MOONTOON_DEBUG_TERM_NAME)
#undef MOONTOON_DEBUG_TERM_NAME
		return Names.Num() > 0 ? FString::Join(Names, TEXT("|")) : FString(TEXT("-"));
	}

	static FString TermName(uint32 Bit)
	{
		return TermNames(1u << Bit);
	}

	static FString LegacyFeatureName(uint32 Id)
	{
#define MOONTOON_DEBUG_FEATURE_NAME(IdName, Module) if (Id == MOON_SHADING_FEATURE_ID_##IdName) { return TEXT(#IdName); }
		MOON_TOON_FEATURE_LIST(MOONTOON_DEBUG_FEATURE_NAME)
#undef MOONTOON_DEBUG_FEATURE_NAME
		return FString::Printf(TEXT("id %u"), Id);
	}

	static FString VariantNames(uint32 Variants)
	{
		TArray<FString> Names;
		if ((Variants & MOON_TOON_VARIANT_KAJIYA_TOON) != 0u)	Names.Add(TEXT("KAJIYA_TOON"));
		if ((Variants & MOON_TOON_VARIANT_STOCKINGS_PBR) != 0u)	Names.Add(TEXT("STOCKINGS_PBR"));
		if ((Variants & MOON_TOON_VARIANT_ANGEL_RING) != 0u)	Names.Add(TEXT("ANGEL_RING"));
		return Names.Num() > 0 ? FString::Join(Names, TEXT("|")) : FString(TEXT("-"));
	}

	static FString FlagName(uint32 Bit)
	{
		switch (Bit)
		{
		case 0: return TEXT("FACIAL_SHADOW_SDF");
		case 1: return TEXT("FACE_OVERLAY_MEMBER");
		case 2: return TEXT("EYEBROW");
		case 3: case 4: case 5: return FString::Printf(TEXT("VARIANT_%u"), Bit - MOON_TOON_FLAG_VARIANT_SHIFT);
		case 6: return TEXT("FACIAL_SDF_PREEVALUATED");
		default: return FString::Printf(TEXT("bit%u"), Bit);
		}
	}

	static FString BitNames(uint32 Word, TFunctionRef<FString(uint32)> NameOf, uint32 BitCount)
	{
		TArray<FString> Names;
		for (uint32 Bit = 0; Bit < BitCount; ++Bit)
		{
			if ((Word & (1u << Bit)) != 0u)
			{
				Names.Add(NameOf(Bit));
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT("|")) : FString(TEXT("-"));
	}

	static FString LightCategoryName(uint32 Category)
	{
		switch (Category)
		{
		case MOONTOON_LIGHT_CATEGORY_AUTO:	return TEXT("AUTO");
		case MOONTOON_LIGHT_CATEGORY_CLOTH:	return TEXT("CLOTH");
		case MOONTOON_LIGHT_CATEGORY_HAIR:	return TEXT("HAIR");
		case MOONTOON_LIGHT_CATEGORY_SKIN:	return TEXT("SKIN");
		default:							return FString::Printf(TEXT("%u"), Category);
		}
	}

	static FString OutlineModeName(uint32 Mode)
	{
		switch (Mode)
		{
		case MOONTOON_OUTLINE_MODE_TOON_LIT:	return TEXT("TOON_LIT");
		case MOONTOON_OUTLINE_MODE_STYLE_LIT:	return TEXT("STYLE_LIT");
		case MOONTOON_OUTLINE_MODE_FLAT:		return TEXT("FLAT");
		default:								return FString::Printf(TEXT("%u"), Mode);
		}
	}

	static FString FormatName(uint32 Format)
	{
		return Format == MOONTOON_RECORD_FORMAT_FULL ? FString(TEXT("FULL")) : (Format == MOONTOON_RECORD_FORMAT_TABLE ? FString(TEXT("TABLE")) : FString::Printf(TEXT("invalid %u"), Format));
	}

	static FString ValidateName(uint32 Bit)
	{
		for (const MoonToonDebug::FValidateInfo& Info : MoonToonDebug::GetValidateInfos())
		{
			if ((uint32)Info.Bit == Bit)
			{
				return Info.Name;
			}
		}
		return FString::Printf(TEXT("bit%u"), Bit);
	}

	// The constant table's channel order: A B C D R G B' E F G' W (MOONTOON_TERM_SLOT_*).
	static const TCHAR* ChannelName(uint32 Channel)
	{
		static const TCHAR* Names[] = { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("R"), TEXT("G"), TEXT("B'"), TEXT("E"), TEXT("F"), TEXT("G'"), TEXT("W") };
		return Channel < UE_ARRAY_COUNT(Names) ? Names[Channel] : TEXT("?");
	}

	// Slot (FToonBuffer member, as the slot tables spell it) -> channel names.
	static void AddSlot(TMap<FString, FString>& Out, const FString& Slot, const FString& Name)
	{
		TArray<FString> Channels;
		if (Slot.StartsWith(TEXT("FeatureInputScalar")))
		{
			Channels.Add(Slot.Right(1));
		}
		else if (Slot == TEXT("FeatureInputVector.x"))	{ Channels.Add(TEXT("R")); }
		else if (Slot == TEXT("FeatureInputVector.y"))	{ Channels.Add(TEXT("G")); }
		else if (Slot == TEXT("FeatureInputVector.z"))	{ Channels.Add(TEXT("B'")); }
		else if (Slot == TEXT("FeatureInputVector.w"))	{ Channels.Add(TEXT("W")); }
		else if (Slot == TEXT("FeatureInputVector.xyz"))	{ Channels.Add(TEXT("R")); Channels.Add(TEXT("G")); Channels.Add(TEXT("B'")); }
		for (const FString& Channel : Channels)
		{
			FString& Names = Out.FindOrAdd(Channel);
			if (!Names.Contains(Name))
			{
				Names = Names.IsEmpty() ? Name : Names + TEXT(" / ") + Name;
			}
		}
	}

	static void AddModuleSlots(const FString& Module, TMap<FString, FString>& Out)
	{
#define MOONTOON_DEBUG_SLOT(Type, Slot, Name, Default, Enc) AddSlot(Out, TEXT(#Slot), TEXT(#Name));
#define MOONTOON_DEBUG_MODULE(ModuleName, SlotTable) if (Module == TEXT(#ModuleName)) { SlotTable(MOONTOON_DEBUG_SLOT) }
		MOON_TOON_MODULE_LIST(MOONTOON_DEBUG_MODULE)
#undef MOONTOON_DEBUG_MODULE
#undef MOONTOON_DEBUG_SLOT
	}

	/** What each feature channel means for this Kind + Terms (+ the angel ring variant). */
	static TMap<FString, FString> SlotNamesFor(uint32 Kind, uint32 Terms, uint32 Variants)
	{
		TMap<FString, FString> Out;
#define MOONTOON_DEBUG_KIND_SLOTS(KindName, Module) if (Kind == MOON_TOON_KIND_##KindName) { AddModuleSlots(TEXT(#Module), Out); }
		MOON_TOON_KIND_LIST(MOONTOON_DEBUG_KIND_SLOTS)
#undef MOONTOON_DEBUG_KIND_SLOTS
#define MOONTOON_DEBUG_TERM_SLOTS(TermName, Module, Bit, Hooks) if ((Terms & (1u << (Bit))) != 0u) { AddModuleSlots(TEXT(#Module), Out); }
		MOON_TOON_SHADING_TERM_LIST(MOONTOON_DEBUG_TERM_SLOTS)
#undef MOONTOON_DEBUG_TERM_SLOTS
		if ((Variants & MOON_TOON_VARIANT_ANGEL_RING) != 0u)
		{
			AddModuleSlots(TEXT("AngelRing"), Out);
		}
		return Out;
	}

	/** Labels of a field's components (fields not listed: one value, or a colour / vector). */
	static TArray<FString> ComponentLabels(const FString& Field)
	{
		static const TMap<FString, TArray<FString>> Labels = []()
		{
			TMap<FString, TArray<FString>> Map;
			Map.Add(TEXT("IsToon"), TArray<FString>{ TEXT("is_toon"), TEXT("has_moon_closure") });
			Map.Add(TEXT("LightCategory"), TArray<FString>{ TEXT("record"), TEXT("resolved"), TEXT("profile_override") });
			Map.Add(TEXT("Outline"), TArray<FString>{ TEXT("hull"), TEXT("record_mode"), TEXT("resolved_mode") });
			Map.Add(TEXT("TonemapWeight"), TArray<FString>{ TEXT("record"), TEXT("resolved") });
			Map.Add(TEXT("ConstantRow"), TArray<FString>{ TEXT("row"), TEXT("valid") });
			Map.Add(TEXT("DisableSelfShadow"), TArray<FString>{ TEXT("record"), TEXT("resolved") });
			Map.Add(TEXT("SdfFaceBits"), TArray<FString>{ TEXT("surface_copy"), TEXT("flags_copy") });
			Map.Add(TEXT("RampOffsets"), TArray<FString>{ TEXT("diffuse"), TEXT("specular") });
			Map.Add(TEXT("RimLight"), TArray<FString>{ TEXT("intensity_record"), TEXT("width_record"), TEXT("intensity"), TEXT("width") });
			Map.Add(TEXT("HairShadowIntensity"), TArray<FString>{ TEXT("record"), TEXT("resolved") });
			Map.Add(TEXT("MetallicSpecularRoughness"), TArray<FString>{ TEXT("metallic"), TEXT("specular"), TEXT("roughness") });
			Map.Add(TEXT("Sdf"), TArray<FString>{ TEXT("left"), TEXT("right") });
			Map.Add(TEXT("SdfExtension"), TArray<FString>{ TEXT("nol_mask"), TEXT("soft_weight"), TEXT("soft_u") });
			Map.Add(TEXT("Closures"), TArray<FString>{ TEXT("count"), TEXT("moon_index"), TEXT("bytes_read"), TEXT("budget") });
			Map.Add(TEXT("LightingChannels"), TArray<FString>{ TEXT("mask"), TEXT("valid") });
			Map.Add(TEXT("BufMainLightMask"), TArray<FString>{ TEXT("shadow"), TEXT("hair"), TEXT("valid") });
			Map.Add(TEXT("BufLitMask"), TArray<FString>{ TEXT("lit"), TEXT("bleed_r"), TEXT("bleed_g"), TEXT("bleed_b") });
			Map.Add(TEXT("BufBleedMask"), TArray<FString>{ TEXT("mc"), TEXT("c") });
			Map.Add(TEXT("BufFaceOverlay"), TArray<FString>{ TEXT("id"), TEXT("depth") });
			Map.Add(TEXT("BufTranslucentPrepass"), TArray<FString>{ TEXT("id"), TEXT("depth") });
			Map.Add(TEXT("BufVelocity"), TArray<FString>{ TEXT("x"), TEXT("y"), TEXT("written") });
			return Map;
		}();
		const TArray<FString>* Found = Labels.Find(Field);
		return Found ? *Found : TArray<FString>();
	}

	/** Components that are floats in a mostly-integer field (stored with asuint). */
	static bool IsFloatComponent(const FString& Field, int32 Component, uint8 Encoding)
	{
		if (Field == TEXT("BufFaceOverlay") || Field == TEXT("BufTranslucentPrepass"))
		{
			return Component == 1;
		}
		if (Field == TEXT("Closures") || Field == TEXT("LightingChannels") || Field == TEXT("IsToon")
			|| Field == TEXT("LightCategory") || Field == TEXT("Outline") || Field == TEXT("ConstantRow") || Field == TEXT("SdfFaceBits"))
		{
			return false;
		}
		return !(Encoding == MOONTOON_DEBUG_ENC_ID || Encoding == MOONTOON_DEBUG_ENC_ENUM || Encoding == MOONTOON_DEBUG_ENC_BITS);
	}

	static FLinearColor Rgb(uint32 Packed)
	{
		return FLinearColor(FColor((Packed >> 16) & 0xFF, (Packed >> 8) & 0xFF, Packed & 0xFF));
	}

	static FString Fmt3(const FVector4f& V)
	{
		return FString::Printf(TEXT("%.4f %.4f %.4f"), V.X, V.Y, V.Z);
	}

	const TCHAR* ActorRowName(int32 Row)
	{
		static const TCHAR* Names[MOONTOON_ACTOR_ROWS_PER_ACTOR] =
		{
			TEXT("pivot.xyz | actor id"),
			TEXT("light override dir.xyz | blend"),
			TEXT("exposure mul | has dir override | translucency | brow strength"),
			TEXT("pivot low.xyz | eye strength"),
			TEXT("rig flags | key elevation | key yaw | key direction weight"),
			TEXT("key world dir.xyz | colour coupling"),
			TEXT("key style colour.rgb | intensity"),
			TEXT("intensity coupling | intensity scale | ambient hue coupling | ambient intensity scale"),
			TEXT("fill yaw | fill pitch | fill intensity | fill band smooth"),
			TEXT("fill colour.rgb | face light blend"),
			TEXT("head forward.xyz | face light yaw"),
			TEXT("head up.xyz | face light pitch"),
			TEXT("self shadow yaw | pitch | weight | -"),
		};
		return Row >= 0 && Row < MOONTOON_ACTOR_ROWS_PER_ACTOR ? Names[Row] : TEXT("?");
	}

	static const TCHAR* ProfileColumnName(int32 Column)
	{
		switch (Column)
		{
		case MOONTOON_PROFILE_COL_RIM:			return TEXT("rim colour.rgb | rim intensity");
		case MOONTOON_PROFILE_COL_STYLE:		return TEXT("rim width | hair shadow | disable shadow map shadow | ramp tint strength");
		case MOONTOON_PROFILE_COL_GI:			return TEXT("diffuse indirect | specular indirect | tonemap weight | exposure mul");
		case MOONTOON_PROFILE_COL_SHADOWCAST:	return TEXT("shadow extinction | specular dark side mirror* | terminator zeroing* | rim dark base boost*");
		case MOONTOON_PROFILE_COL_TONE:			return TEXT("ramp hue mode* | three tone* | deep dark scale* | lit scale*");
		case MOONTOON_PROFILE_COL_TONE2:		return TEXT("deep dark saturation* | shadow side desaturation* | camera fill* | backlight compensation*");
		case MOONTOON_PROFILE_COL_SEGMENT:		return TEXT("specular segment* | rim segment* | secondary segment* | time of day ramp*");
		case MOONTOON_PROFILE_COL_MISC:			return TEXT("light category* | character grading* | category light* | specular camera bias*");
		default:								return TEXT("?");
		}
	}

	/** The style fields a profile can override (columns 3..7) and where the view value sits in the style buffer. */
	struct FStyleField { const TCHAR* Name; int32 Slot; int32 SlotComponent; int32 ProfileColumn; int32 ProfileComponent; };
	static const FStyleField GStyleFields[] =
	{
		{ TEXT("RampHueMode"),					MOONTOON_STYLE_SLOT_TONE0,		0, MOONTOON_PROFILE_COL_TONE,		0 },
		{ TEXT("HueLuminanceCap"),				MOONTOON_STYLE_SLOT_TONE0,		1, -1, 0 },
		{ TEXT("ShadowSideLightDesaturation"),	MOONTOON_STYLE_SLOT_TONE0,		2, MOONTOON_PROFILE_COL_TONE2,		1 },
		{ TEXT("CameraFillStrength"),			MOONTOON_STYLE_SLOT_TONE0,		3, MOONTOON_PROFILE_COL_TONE2,		2 },
		{ TEXT("ThreeToneEnable"),				MOONTOON_STYLE_SLOT_TONE1,		0, MOONTOON_PROFILE_COL_TONE,		1 },
		{ TEXT("DeepDarkScale"),				MOONTOON_STYLE_SLOT_TONE1,		1, MOONTOON_PROFILE_COL_TONE,		2 },
		{ TEXT("DeepDarkSaturation"),			MOONTOON_STYLE_SLOT_TONE1,		2, MOONTOON_PROFILE_COL_TONE2,		0 },
		{ TEXT("LitScale"),						MOONTOON_STYLE_SLOT_TONE1,		3, MOONTOON_PROFILE_COL_TONE,		3 },
		{ TEXT("BacklightCompensation"),		MOONTOON_STYLE_SLOT_TONE2,		0, MOONTOON_PROFILE_COL_TONE2,		3 },
		{ TEXT("SpecularCameraBias"),			MOONTOON_STYLE_SLOT_TONE2,		1, MOONTOON_PROFILE_COL_MISC,		3 },
		{ TEXT("SpecularDarkSideMirror"),		MOONTOON_STYLE_SLOT_TONE2,		2, MOONTOON_PROFILE_COL_SHADOWCAST,	1 },
		{ TEXT("SpecularTerminatorZeroing"),	MOONTOON_STYLE_SLOT_TONE2,		3, MOONTOON_PROFILE_COL_SHADOWCAST,	2 },
		{ TEXT("SpecularSegmentStrength"),		MOONTOON_STYLE_SLOT_SEGMENT,	0, MOONTOON_PROFILE_COL_SEGMENT,	0 },
		{ TEXT("RimSegmentStrength"),			MOONTOON_STYLE_SLOT_SEGMENT,	1, MOONTOON_PROFILE_COL_SEGMENT,	1 },
		{ TEXT("SecondarySegmentStrength"),		MOONTOON_STYLE_SLOT_SEGMENT,	2, MOONTOON_PROFILE_COL_SEGMENT,	2 },
		{ TEXT("TimeOfDayRampEnable"),			MOONTOON_STYLE_SLOT_TIMEOFDAY,	3, MOONTOON_PROFILE_COL_SEGMENT,	3 },
		{ TEXT("RimDarkBaseBoost"),				MOONTOON_STYLE_SLOT_RIM_EXTRA,	0, MOONTOON_PROFILE_COL_SHADOWCAST,	3 },
		{ TEXT("RimFadeStartDistance"),			MOONTOON_STYLE_SLOT_RIM_EXTRA,	1, -1, 0 },
		{ TEXT("RimFadeEndDistance"),			MOONTOON_STYLE_SLOT_RIM_EXTRA,	2, -1, 0 },
		{ TEXT("SceneShadowLift"),				MOONTOON_STYLE_SLOT_SHADOW,		0, -1, 0 },
		{ TEXT("OutlineWidthScale"),			MOONTOON_STYLE_SLOT_OUTLINE,	0, -1, 0 },
		{ TEXT("OutlineDefaultShadingMode"),	MOONTOON_STYLE_SLOT_OUTLINE,	3, -1, 0 },
		{ TEXT("FacialShadowSoftness"),			MOONTOON_STYLE_SLOT_FACE,		0, -1, 0 },
	};

	static float Component(const FVector4f& V, int32 C)
	{
		return C == 0 ? V.X : (C == 1 ? V.Y : (C == 2 ? V.Z : V.W));
	}
}

using namespace MoonToonDebugNames;

// -------------------------------------------------------------------------------------------------

FString UMoonToonDebugSubsystem::NameForValue(const FString& ViewName, uint32 Value, bool bPerBit) const
{
	const FString View = ViewName.ToLower().Replace(TEXT("."), TEXT("_"));
	if (bPerBit)
	{
		if (View == TEXT("validate"))	return ValidateName(Value);
		if (View == TEXT("terms"))		return TermName(Value);
		if (View == TEXT("flags"))		return FlagName(Value);
		if (View == TEXT("ctmask"))		return ChannelName(Value);
		if (View == TEXT("lightchannel"))	return FString::Printf(TEXT("channel %u"), Value);
		if (View == TEXT("variants"))	return VariantNames(1u << Value);
		return FString::Printf(TEXT("bit %u"), Value);
	}
	if (View == TEXT("profile"))
	{
		if (Value == 0u) return TEXT("no profile");
		const FString* Name = ProfileNames.Find((int32)Value);
		return Name ? FPaths::GetBaseFilename(*Name) : FString(TEXT("unregistered profile"));
	}
	if (View == TEXT("actor") || View == TEXT("buf_translucentprepass") || View == TEXT("buf_faceoverlay"))
	{
		const uint32 ActorId = View == TEXT("buf_faceoverlay") ? Value / 256u : Value;
		if (ActorId == 0u) return TEXT("no actor id");
		const TWeakObjectPtr<AActor>* Actor = ActorsById.Find((int32)ActorId);
		const FString ActorName = (Actor && Actor->IsValid()) ? (*Actor)->GetActorNameOrLabel() : FString(TEXT("unknown actor"));
		return View == TEXT("buf_faceoverlay") ? FString::Printf(TEXT("%s, class %u"), *ActorName, Value % 256u) : ActorName;
	}
	if (View == TEXT("rampidx"))
	{
		const FString* Name = RampSliceNames.Find((int32)Value);
		return Name ? *Name : FString();
	}
	if (View == TEXT("ctrow"))
	{
		if (Value >= (uint32)MOONTOON_CONSTANT_ROW_INVALID) return TEXT("no row");
		const FString* Name = ConstantRowNames.Find((int32)Value);
		return Name ? *Name : FString();
	}
	if (View == TEXT("kind"))		return KindName(Value);
	if (View == TEXT("feature"))	return LegacyFeatureName(Value);
	if (View == TEXT("terms"))		return TermNames(Value);
	if (View == TEXT("variants"))	return VariantNames(Value);
	if (View == TEXT("flags"))		return BitNames(Value, [](uint32 Bit) { return FlagName(Bit); }, 8);
	if (View == TEXT("validate"))	return BitNames(Value, [](uint32 Bit) { return ValidateName(Bit); }, MOONTOON_DEBUG_VALIDATE_COUNT);
	if (View == TEXT("ctmask"))		return BitNames(Value, [](uint32 Bit) { return FString(ChannelName(Bit)); }, MOONTOON_CONSTANT_CHANNEL_COUNT);
	if (View == TEXT("lightcat"))	return LightCategoryName(Value);
	if (View == TEXT("outline"))	return OutlineModeName(Value);
	if (View == TEXT("format"))		return FormatName(Value);
	if (View == TEXT("stencil"))	return FString::Printf(TEXT("stencil %u"), Value);
	if (View == TEXT("closures"))	return FString::Printf(TEXT("%u closure(s)"), Value);
	if (View == TEXT("sdfbit"))		return (Value & 0xFFu) == (Value >> 8u) ? TEXT("copies agree") : TEXT("copies DISAGREE");
	return FString();
}

FMoonToonRegionStats UMoonToonDebugSubsystem::BuildStats(const MoonToonDebug::FRegionStatsResult& Result, const FString& ViewName) const
{
	FMoonToonRegionStats Out;
	Out.ViewName = ViewName;
	Out.Error = Result.Error;
	Out.bValid = Result.bValid;
	Out.OverflowPixels = (int32)Result.OverflowPixels;
	const FIntRect& Rect = Result.ViewRect;
	const FIntRect& Unscaled = Result.UnscaledViewRect;
	for (const MoonToonDebug::FRegionValue& Value : Result.Values)
	{
		FMoonToonRegionValue& V = Out.Values.AddDefaulted_GetRef();
		V.Key = Value.Key;
		V.bPerBit = Value.bPerBit;
		V.Name = NameForValue(ViewName, Value.Key, Value.bPerBit);
		V.PixelCount = (int32)Value.PixelCount;
		V.BoundsMin = Value.Bounds.Min;
		V.BoundsMax = Value.Bounds.Max;
		V.Sample = Value.Sample;
		if (Rect.Width() > 0 && Rect.Height() > 0)
		{
			V.SampleViewport = Unscaled.Min + FIntPoint(
				FMath::FloorToInt((Value.Sample.X + 0.5f) * Unscaled.Width() / (float)Rect.Width()),
				FMath::FloorToInt((Value.Sample.Y + 0.5f) * Unscaled.Height() / (float)Rect.Height()));
		}
		if (!Value.bPerBit)
		{
			V.Swatch = Rgb(Result.Encoding == MOONTOON_DEBUG_ENC_ENUM ? MoonToonDebug::GetEnumColor(Value.Key) : MoonToonDebug::GetIdColor(Value.Key));
		}
	}
	Out.Json = ToJson(Out);
	return Out;
}

FMoonToonPixelReport UMoonToonDebugSubsystem::BuildReport(const MoonToonDebug::FInspectResult& R, FVector2D RequestPosition, const FString& Space, FIntPoint ViewportPixel,
	FIntPoint ViewportSize, UObject* HitMaterial, int32 HitSlot, UObject* HitComponent) const
{
	RefreshNameCaches(false);

	FMoonToonPixelReport Out;
	Out.RequestPosition = RequestPosition;
	Out.RequestSpace = Space;
	Out.ViewportPixel = ViewportPixel;
	Out.ViewportSize = ViewportSize;
	if (!R.bValid || R.Fields.Num() < MOONTOON_DEBUG_FIELD_COUNT)
	{
		Out.Error = R.Error.IsEmpty() ? FString(TEXT("no result")) : R.Error;
		Out.Json = ToJson(Out);
		return Out;
	}
	Out.bValid = true;
	Out.FrameNumber = (int64)R.FrameNumber;
	Out.RenderPixel = R.RenderPixel - R.ViewRect.Min;
	Out.RenderSize = R.ViewRect.Size();
	Out.ScreenPercentage = R.UnscaledViewRect.Width() > 0 ? 100.0f * R.ViewRect.Width() / (float)R.UnscaledViewRect.Width() : 100.0f;
	if (!Space.Equals(TEXT("viewport"), ESearchCase::IgnoreCase) && R.ViewRect.Width() > 0 && R.ViewRect.Height() > 0)
	{
		Out.ViewportPixel = R.UnscaledViewRect.Min + FIntPoint(
			FMath::FloorToInt((Out.RenderPixel.X + 0.5f) * R.UnscaledViewRect.Width() / (float)R.ViewRect.Width()),
			FMath::FloorToInt((Out.RenderPixel.Y + 0.5f) * R.UnscaledViewRect.Height() / (float)R.ViewRect.Height()));
	}
	if (HitMaterial)
	{
		Out.MaterialPath = HitMaterial->GetPathName();
		Out.MaterialSlot = HitSlot;
	}
	if (HitComponent)
	{
		Out.ComponentPath = HitComponent->GetPathName();
	}

	const auto F = [&R](int32 Field) -> const FUintVector4& { return R.Fields[Field]; };
	const auto FX = [&R](int32 Field, int32 C) { return AsFloat(C == 0 ? R.Fields[Field].X : (C == 1 ? R.Fields[Field].Y : (C == 2 ? R.Fields[Field].Z : R.Fields[Field].W))); };
	const auto Add = [&Out](const TCHAR* Section, const FString& Name, const FString& Value, double Number, const FString& Source = FString(), FLinearColor Swatch = FLinearColor::Transparent)
	{
		FMoonToonDebugField& Field = Out.Fields.AddDefaulted_GetRef();
		Field.Section = Section;
		Field.Name = Name;
		Field.Value = Value;
		Field.Number = Number;
		Field.Source = Source;
		Field.Swatch = Swatch;
	};

	Out.bIsToon = F(MOONTOON_DEBUG_FIELD_IsToon).X != 0u;
	Out.ActorId = (int32)F(MOONTOON_DEBUG_FIELD_ActorId).X;
	Out.ProfileId = (int32)F(MOONTOON_DEBUG_FIELD_ProfileId).X;
	Out.ValidateBits = (int32)F(MOONTOON_DEBUG_FIELD_Validate).X;
	for (uint32 Bit = 0; Bit < MOONTOON_DEBUG_VALIDATE_COUNT; ++Bit)
	{
		if ((Out.ValidateBits & (1 << Bit)) != 0)
		{
			Out.ValidateNames.Add(ValidateName(Bit));
		}
	}
	if (const TWeakObjectPtr<AActor>* Actor = ActorsById.Find(Out.ActorId))
	{
		if (Actor->IsValid())
		{
			Out.ActorName = (*Actor)->GetActorNameOrLabel();
			Out.ActorPath = (*Actor)->GetPathName();
		}
	}
	if (const FString* Profile = ProfileNames.Find(Out.ProfileId))
	{
		Out.ProfilePath = *Profile;
	}
	const FString ProfileSource = Out.ProfileId > 0
		? FString::Printf(TEXT("profile:%s"), Out.ProfilePath.IsEmpty() ? *FString::Printf(TEXT("id %d"), Out.ProfileId) : *FPaths::GetBaseFilename(Out.ProfilePath))
		: FString(TEXT("material"));

	// --- record ---------------------------------------------------------------------------------
	const FUintVector4& Closures = F(MOONTOON_DEBUG_FIELD_Closures);
	Add(TEXT("record"), TEXT("is_toon"), Out.bIsToon ? TEXT("yes") : TEXT("no"), Out.bIsToon ? 1 : 0);
	Add(TEXT("record"), TEXT("has_moon_closure"), F(MOONTOON_DEBUG_FIELD_IsToon).Y != 0u ? TEXT("yes") : TEXT("no"), F(MOONTOON_DEBUG_FIELD_IsToon).Y);
	Add(TEXT("record"), TEXT("closures"), FString::Printf(TEXT("%u (moon at %s, %u / %u bytes)"), Closures.X,
		Closures.Y == 0xFFu ? TEXT("-") : *FString::FromInt((int32)Closures.Y), Closures.Z, Closures.W), Closures.X);
	const uint32 Format = F(MOONTOON_DEBUG_FIELD_RecordFormat).X;
	Add(TEXT("record"), TEXT("format"), FormatName(Format), Format);
	const FUintVector4& Row = F(MOONTOON_DEBUG_FIELD_ConstantRow);
	const FString* RowMaterials = ConstantRowNames.Find((int32)Row.X);
	const bool bTableRecord = Out.bIsToon && Format == MOONTOON_RECORD_FORMAT_TABLE;
	Add(TEXT("record"), TEXT("constant_row"), !bTableRecord ? FString(TEXT("- (no constant row: not a TABLE record)"))
		: Row.X >= (uint32)MOONTOON_CONSTANT_ROW_INVALID ? FString(TEXT("none"))
		: FString::Printf(TEXT("%u%s%s"), Row.X, Row.Y ? TEXT("") : TEXT(" (NOT VALID)"), RowMaterials ? *(TEXT(" <- ") + *RowMaterials) : TEXT("")), Row.X);
	const uint32 PerPixelMask = F(MOONTOON_DEBUG_FIELD_PerPixelMask).X;
	Add(TEXT("record"), TEXT("per_pixel_channels"), BitNames(PerPixelMask, [](uint32 Bit) { return FString(ChannelName(Bit)); }, MOONTOON_CONSTANT_CHANNEL_COUNT), PerPixelMask);
	for (int32 It = 0; It < R.RawRecord.Num(); ++It)
	{
		Add(TEXT("record"), FString::Printf(TEXT("Data%d"), It), FString::Printf(TEXT("0x%08X"), R.RawRecord[It]), R.RawRecord[It]);
	}

	// --- feature ----------------------------------------------------------------------------------
	const uint32 Kind = F(MOONTOON_DEBUG_FIELD_Kind).X;
	const uint32 Terms = F(MOONTOON_DEBUG_FIELD_Terms).X;
	const uint32 Variants = F(MOONTOON_DEBUG_FIELD_Variants).X;
	const uint32 Flags = F(MOONTOON_DEBUG_FIELD_ToonFlags).X;
	Add(TEXT("feature"), TEXT("kind"), KindName(Kind), Kind, TEXT("material"), Rgb(MoonToonDebug::GetEnumColor(Kind)));
	Add(TEXT("feature"), TEXT("terms"), TermNames(Terms), Terms, TEXT("material"));
	Add(TEXT("feature"), TEXT("variants"), VariantNames(Variants), Variants, TEXT("material"));
	const uint32 LegacyId = F(MOONTOON_DEBUG_FIELD_LegacyFeatureId).X;
	Add(TEXT("feature"), TEXT("legacy_feature"), LegacyFeatureName(LegacyId), LegacyId);
	Add(TEXT("feature"), TEXT("flags"), BitNames(Flags, [](uint32 Bit) { return FlagName(Bit); }, 8), Flags);
	{
		const TMap<FString, FString> Slots = SlotNamesFor(Kind, Terms, Variants);
		struct FChannelSlot { int32 Field; int32 Component; uint32 Channel; };
		static const FChannelSlot Channels[] =
		{
			{ MOONTOON_DEBUG_FIELD_ChannelsABCD, 0, 0 }, { MOONTOON_DEBUG_FIELD_ChannelsABCD, 1, 1 }, { MOONTOON_DEBUG_FIELD_ChannelsABCD, 2, 2 }, { MOONTOON_DEBUG_FIELD_ChannelsABCD, 3, 3 },
			{ MOONTOON_DEBUG_FIELD_ChannelsVector, 0, 4 }, { MOONTOON_DEBUG_FIELD_ChannelsVector, 1, 5 }, { MOONTOON_DEBUG_FIELD_ChannelsVector, 2, 6 },
			{ MOONTOON_DEBUG_FIELD_ChannelsEFGW, 0, 7 }, { MOONTOON_DEBUG_FIELD_ChannelsEFGW, 1, 8 }, { MOONTOON_DEBUG_FIELD_ChannelsEFGW, 2, 9 }, { MOONTOON_DEBUG_FIELD_ChannelsEFGW, 3, 10 },
		};
		for (const FChannelSlot& Channel : Channels)
		{
			const FString Name = ChannelName(Channel.Channel);
			const FString* Slot = Slots.Find(Name);
			const bool bPerPixel = (PerPixelMask & (1u << Channel.Channel)) != 0u;
			const FString Source = Format == MOONTOON_RECORD_FORMAT_TABLE
				? (bPerPixel ? FString(TEXT("per pixel (record)")) : FString::Printf(TEXT("constant row %u"), Row.X))
				: FString(TEXT("record"));
			const float Value = FX(Channel.Field, Channel.Component);
			Add(TEXT("feature"), FString::Printf(TEXT("ch %s%s"), *Name, Slot ? *FString::Printf(TEXT(" (%s)"), **Slot) : TEXT(" (unused)")),
				FString::Printf(TEXT("%.4f"), Value), Value, Source);
		}
	}

	// --- surface ----------------------------------------------------------------------------------
	const auto Color = [&FX](int32 Field) { return FVector4f(FX(Field, 0), FX(Field, 1), FX(Field, 2), 1.0f); };
	const FVector4f Base = Color(MOONTOON_DEBUG_FIELD_BaseColor);
	Add(TEXT("surface"), TEXT("base_color"), Fmt3(Base), Base.X, TEXT("material"), FLinearColor(Base.X, Base.Y, Base.Z));
	Add(TEXT("surface"), TEXT("metallic"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 0)), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 0), TEXT("material"));
	Add(TEXT("surface"), TEXT("specular"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 1)), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 1), TEXT("material"));
	Add(TEXT("surface"), TEXT("roughness"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 2)), FX(MOONTOON_DEBUG_FIELD_MetallicSpecularRoughness, 2), TEXT("material"));
	Add(TEXT("surface"), TEXT("anisotropy"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_Anisotropy, 0)), FX(MOONTOON_DEBUG_FIELD_Anisotropy, 0), TEXT("material"));
	Add(TEXT("surface"), TEXT("tangent"), Fmt3(Color(MOONTOON_DEBUG_FIELD_Tangent)), FX(MOONTOON_DEBUG_FIELD_Tangent, 0), TEXT("material"));
	Add(TEXT("surface"), TEXT("normal"), Fmt3(Color(MOONTOON_DEBUG_FIELD_Normal)), FX(MOONTOON_DEBUG_FIELD_Normal, 0));
	const FVector4f Spec = Color(MOONTOON_DEBUG_FIELD_SpecularColor);
	Add(TEXT("surface"), TEXT("toon_specular_color"), Fmt3(Spec), Spec.X, TEXT("material"), FLinearColor(Spec.X, Spec.Y, Spec.Z));
	const FVector4f Shadow = Color(MOONTOON_DEBUG_FIELD_ShadowColor);
	Add(TEXT("surface"), TEXT("main_light_shadow_color"), Fmt3(Shadow), Shadow.X, TEXT("material"), FLinearColor(Shadow.X, Shadow.Y, Shadow.Z));
	Add(TEXT("surface"), TEXT("reflection_intensity"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_ReflectionIntensity, 0)), FX(MOONTOON_DEBUG_FIELD_ReflectionIntensity, 0), TEXT("material"));
	Add(TEXT("surface"), TEXT("rim_intensity"), FString::Printf(TEXT("%.4f (record %.4f)"), FX(MOONTOON_DEBUG_FIELD_RimLight, 2), FX(MOONTOON_DEBUG_FIELD_RimLight, 0)),
		FX(MOONTOON_DEBUG_FIELD_RimLight, 2), Out.ProfileId > 0 ? ProfileSource : FString(TEXT("material pin")));
	Add(TEXT("surface"), TEXT("rim_width"), FString::Printf(TEXT("%.4f (record %.4f)"), FX(MOONTOON_DEBUG_FIELD_RimLight, 3), FX(MOONTOON_DEBUG_FIELD_RimLight, 1)),
		FX(MOONTOON_DEBUG_FIELD_RimLight, 3), Out.ProfileId > 0 ? ProfileSource : FString(TEXT("material pin")));
	Add(TEXT("surface"), TEXT("hair_shadow_intensity"), FString::Printf(TEXT("%.4f (record %.4f)"), FX(MOONTOON_DEBUG_FIELD_HairShadowIntensity, 1), FX(MOONTOON_DEBUG_FIELD_HairShadowIntensity, 0)),
		FX(MOONTOON_DEBUG_FIELD_HairShadowIntensity, 1), Out.ProfileId > 0 ? ProfileSource : FString(TEXT("record neutral")));
	Add(TEXT("surface"), TEXT("disable_self_shadow"), FString::Printf(TEXT("%.0f (record %.0f)"), FX(MOONTOON_DEBUG_FIELD_DisableSelfShadow, 1), FX(MOONTOON_DEBUG_FIELD_DisableSelfShadow, 0)),
		FX(MOONTOON_DEBUG_FIELD_DisableSelfShadow, 1), Out.ProfileId > 0 ? ProfileSource : FString(TEXT("record neutral")));
	const FUintVector4& SdfBits = F(MOONTOON_DEBUG_FIELD_SdfFaceBits);
	Add(TEXT("surface"), TEXT("sdf_face_bit"), FString::Printf(TEXT("surface %u / flags %u%s"), SdfBits.X, SdfBits.Y, SdfBits.X != SdfBits.Y ? TEXT("  DISAGREE") : TEXT("")), SdfBits.Y);
	Add(TEXT("surface"), TEXT("stencil"), FString::FromInt((int32)F(MOONTOON_DEBUG_FIELD_Stencil).X), F(MOONTOON_DEBUG_FIELD_Stencil).X, TEXT("material"));
	Add(TEXT("surface"), TEXT("ray_tracing_shadow_flag"), FString::FromInt((int32)F(MOONTOON_DEBUG_FIELD_RayTracingShadowFlag).X), F(MOONTOON_DEBUG_FIELD_RayTracingShadowFlag).X, TEXT("material"));
	const uint32 DiffuseRamp = F(MOONTOON_DEBUG_FIELD_DiffuseRampIndex).X;
	const uint32 SpecularRamp = F(MOONTOON_DEBUG_FIELD_SpecularRampIndex).X;
	const FString* DiffuseRampName = RampSliceNames.Find((int32)DiffuseRamp);
	const FString* SpecularRampName = SpecularSliceNames.Find((int32)SpecularRamp);
	Add(TEXT("surface"), TEXT("diffuse_ramp"), FString::Printf(TEXT("%u%s"), DiffuseRamp, DiffuseRampName ? *(TEXT(" <- ") + *DiffuseRampName) : TEXT("")), DiffuseRamp, TEXT("material"),
		Rgb(MoonToonDebug::GetIdColor(DiffuseRamp)));
	Add(TEXT("surface"), TEXT("specular_ramp"), FString::Printf(TEXT("%u%s"), SpecularRamp, SpecularRampName ? *(TEXT(" <- ") + *SpecularRampName) : TEXT("")), SpecularRamp, TEXT("material"));
	Add(TEXT("surface"), TEXT("diffuse_ramp_offset"), FString::Printf(TEXT("%.4f (0.5 = none)"), FX(MOONTOON_DEBUG_FIELD_RampOffsets, 0)), FX(MOONTOON_DEBUG_FIELD_RampOffsets, 0), TEXT("material"));
	Add(TEXT("surface"), TEXT("specular_ramp_offset"), FString::Printf(TEXT("%.4f (0.5 = none)"), FX(MOONTOON_DEBUG_FIELD_RampOffsets, 1)), FX(MOONTOON_DEBUG_FIELD_RampOffsets, 1), TEXT("material"));

	// --- meta ---------------------------------------------------------------------------------------
	Add(TEXT("meta"), TEXT("actor_id"), FString::Printf(TEXT("%d%s"), Out.ActorId, Out.ActorId == 0 ? TEXT(" (no MoonToonActorComponent)") : *(TEXT(" ") + Out.ActorName)),
		Out.ActorId, TEXT("MoonToonActorComponent"), Rgb(MoonToonDebug::GetIdColor((uint32)Out.ActorId)));
	Add(TEXT("meta"), TEXT("profile_id"), FString::Printf(TEXT("%d %s"), Out.ProfileId, *Out.ProfilePath), Out.ProfileId, TEXT("material"), Rgb(MoonToonDebug::GetIdColor((uint32)Out.ProfileId)));
	const FUintVector4& Category = F(MOONTOON_DEBUG_FIELD_LightCategory);
	Add(TEXT("meta"), TEXT("light_category"), FString::Printf(TEXT("%s (record %s)"), *LightCategoryName(Category.Y), *LightCategoryName(Category.X)), Category.Y,
		Category.Z ? ProfileSource : (Category.X != MOONTOON_LIGHT_CATEGORY_AUTO ? FString(TEXT("material (node)")) : FString(TEXT("derived (kind / terms)"))));
	const FUintVector4& Outline = F(MOONTOON_DEBUG_FIELD_Outline);
	Add(TEXT("meta"), TEXT("outline"), Outline.X ? FString::Printf(TEXT("hull, %s (record %s)"), *OutlineModeName(Outline.Z), *OutlineModeName(Outline.Y)) : FString(TEXT("not a hull")), Outline.X,
		Outline.X && Outline.Y == 0u && Outline.Z != 0u ? FString(TEXT("preset (OUTLINE.w)")) : FString(TEXT("material (node)")));
	Add(TEXT("meta"), TEXT("tonemap_weight"), FString::Printf(TEXT("%.4f (record %.4f)"), FX(MOONTOON_DEBUG_FIELD_TonemapWeight, 1), FX(MOONTOON_DEBUG_FIELD_TonemapWeight, 0)),
		FX(MOONTOON_DEBUG_FIELD_TonemapWeight, 1), Out.ProfileId > 0 ? ProfileSource : FString(TEXT("record neutral")));
	if (!Out.MaterialPath.IsEmpty())
	{
		Add(TEXT("meta"), TEXT("material"), FString::Printf(TEXT("%s (slot %d)"), *Out.MaterialPath, Out.MaterialSlot), Out.MaterialSlot, TEXT("hit proxy"));
	}

	// --- face SDF -------------------------------------------------------------------------------------
	Add(TEXT("sdf"), TEXT("left / right"), FString::Printf(TEXT("%.5f / %.5f"), FX(MOONTOON_DEBUG_FIELD_Sdf, 0), FX(MOONTOON_DEBUG_FIELD_Sdf, 1)), FX(MOONTOON_DEBUG_FIELD_Sdf, 0), TEXT("material"));
	Add(TEXT("sdf"), TEXT("nol_mask"), FString::Printf(TEXT("%.4f"), FX(MOONTOON_DEBUG_FIELD_SdfExtension, 0)), FX(MOONTOON_DEBUG_FIELD_SdfExtension, 0), TEXT("material"));
	Add(TEXT("sdf"), TEXT("soft_weight / u"), FString::Printf(TEXT("%.0f / %.4f"), FX(MOONTOON_DEBUG_FIELD_SdfExtension, 1), FX(MOONTOON_DEBUG_FIELD_SdfExtension, 2)), FX(MOONTOON_DEBUG_FIELD_SdfExtension, 1), TEXT("material"));

	// --- validate ---------------------------------------------------------------------------------------
	Add(TEXT("validate"), TEXT("self_check"), Out.ValidateBits == 0 ? FString(TEXT("ok")) : FString::Join(Out.ValidateNames, TEXT(", ")), Out.ValidateBits);

	// --- pixel ----------------------------------------------------------------------------------------------
	const FUintVector4& Channels = F(MOONTOON_DEBUG_FIELD_LightingChannels);
	Add(TEXT("pixel"), TEXT("lighting_channels"), Channels.Y ? FString::Printf(TEXT("0x%02X"), Channels.X) : FString(TEXT("all (view uses no lighting channels)")), Channels.X);
	Add(TEXT("pixel"), TEXT("scene_depth_cm"), FString::Printf(TEXT("%.2f"), FX(MOONTOON_DEBUG_FIELD_SceneDepth, 0)), FX(MOONTOON_DEBUG_FIELD_SceneDepth, 0));

	// --- buffers -------------------------------------------------------------------------------------------------
	for (const MoonToonDebug::FFieldInfo& Info : MoonToonDebug::GetFieldInfos())
	{
		if (!Info.Name.StartsWith(TEXT("Buf")))
		{
			continue;
		}
		const TArray<FString> Labels = ComponentLabels(Info.Name);
		TArray<FString> Parts;
		const int32 Count = Labels.Num() > 0 ? Labels.Num() : 3;
		for (int32 C = 0; C < Count; ++C)
		{
			const uint32 Word = C == 0 ? R.Fields[Info.Index].X : (C == 1 ? R.Fields[Info.Index].Y : (C == 2 ? R.Fields[Info.Index].Z : R.Fields[Info.Index].W));
			const FString Label = Labels.IsValidIndex(C) ? Labels[C] : FString::Printf(TEXT("%c"), TEXT("rgb")[C]);
			Parts.Add(IsFloatComponent(Info.Name, C, Info.Encoding)
				? FString::Printf(TEXT("%s %.4f"), *Label, AsFloat(Word))
				: FString::Printf(TEXT("%s %u"), *Label, Word));
		}
		Add(TEXT("buffers"), Info.Name.RightChop(3), FString::Join(Parts, TEXT("  ")), IsFloatComponent(Info.Name, 0, Info.Encoding) ? AsFloat(R.Fields[Info.Index].X) : R.Fields[Info.Index].X);
	}

	// --- profile row (raw) -------------------------------------------------------------------------------------
	for (int32 Column = 0; Column < R.ProfileRow.Num(); ++Column)
	{
		const FVector4f& V = R.ProfileRow[Column];
		Add(TEXT("profile"), ProfileColumnName(Column), FString::Printf(TEXT("%.4f %.4f %.4f %.4f"), V.X, V.Y, V.Z, V.W), V.X, ProfileSource);
	}

	// --- style chain -------------------------------------------------------------------------------------------
	if (R.Style.Num() == MOONTOON_STYLE_FLOAT4_COUNT)
	{
		const uint32 StyleFlags = (uint32)FMath::Max(R.Style[MOONTOON_STYLE_SLOT_FLAGS].X, 0.0f);
		const bool bStyleOn = (StyleFlags & MOONTOON_STYLE_FLAG_ENABLE) != 0u;
		const TArray<FMoonToonStyleLayer> Layers = GetStyleLayers();
		for (const FStyleField& Style : GStyleFields)
		{
			float Value = Component(R.Style[Style.Slot], Style.SlotComponent);
			FString Source;
			if (Style.ProfileColumn >= 0 && R.ProfileRow.IsValidIndex(Style.ProfileColumn)
				&& Component(R.ProfileRow[Style.ProfileColumn], Style.ProfileComponent) >= 0.0f)
			{
				Value = Component(R.ProfileRow[Style.ProfileColumn], Style.ProfileComponent);
				Source = ProfileSource;
			}
			else if (bStyleOn)
			{
				for (int32 It = Layers.Num() - 1; It >= 0 && Source.IsEmpty(); --It)
				{
					for (const FString& Overridden : Layers[It].OverriddenFields)
					{
						if (Overridden.Contains(Style.Name, ESearchCase::IgnoreCase) || FString(Style.Name).Contains(Overridden, ESearchCase::IgnoreCase))
						{
							Source = FString::Printf(TEXT("preset:%s @ %s"), *FPaths::GetBaseFilename(Layers[It].PresetPath), *Layers[It].Volume);
							break;
						}
					}
				}
				if (Source.IsEmpty())
				{
					Source = TEXT("preset (blended; no single volume names it)");
				}
			}
			else
			{
				// No preset: the shaders use the code defaults ("change nothing"), the buffer is all zero.
				double Default = 0.0;
				if (MoonToonStyleCodeDefault(Style.Name, Default))
				{
					Value = (float)Default;
					Source = TEXT("code default (no style preset)");
				}
				else
				{
					Source = TEXT("code default (no style preset; not found by name, buffer value shown)");
				}
			}
			Add(TEXT("style"), Style.Name, FString::Printf(TEXT("%.4f"), Value), Value, Source);
		}
	}

	// --- actor table block -------------------------------------------------------------------------------------
	for (int32 RowIndex = 0; RowIndex < R.ActorRows.Num(); ++RowIndex)
	{
		const FVector4f& V = R.ActorRows[RowIndex];
		Add(TEXT("actor_table"), ActorRowName(RowIndex), FString::Printf(TEXT("%.4f %.4f %.4f %.4f"), V.X, V.Y, V.Z, V.W), V.X,
			Out.ActorId == 0 ? FString(TEXT("block 0 = the view's light rig")) : FString());
	}

	// --- stage ledger ---------------------------------------------------------------------------------------------
	const float Exposure = R.PreExposure > 0.0f ? R.PreExposure : 1.0f;
	const TConstArrayView<MoonToonDebug::FFieldInfo> StageInfos = MoonToonDebug::GetStageInfos();
	FVector4f Previous(0.0f, 0.0f, 0.0f, 0.0f);
	bool bHavePrevious = false;
	FVector4f Direct(0.0f, 0.0f, 0.0f, 0.0f);
	bool bHaveDirect = false;
	for (const MoonToonDebug::FFieldInfo& Stage : StageInfos)
	{
		if ((R.StageMask & (1u << Stage.Index)) == 0u || !R.Stages.IsValidIndex(Stage.Index))
		{
			continue;
		}
		const bool bDisplay = Stage.Index == MOONTOON_DEBUG_STAGE_PostTonemap;
		const FVector4f Value = bDisplay ? R.Stages[Stage.Index] : R.Stages[Stage.Index] / Exposure;
		FString Text = bDisplay ? FString::Printf(TEXT("%.4f %.4f %.4f (display, 8-bit %d %d %d)"), Value.X, Value.Y, Value.Z,
			FMath::RoundToInt(FMath::Clamp(Value.X, 0.0f, 1.0f) * 255.0f), FMath::RoundToInt(FMath::Clamp(Value.Y, 0.0f, 1.0f) * 255.0f), FMath::RoundToInt(FMath::Clamp(Value.Z, 0.0f, 1.0f) * 255.0f))
			: Fmt3(Value);
		double Delta = 0.0;
		if (bHavePrevious && !bDisplay && Stage.Index != MOONTOON_DEBUG_STAGE_PreTonemap)
		{
			const FVector4f D = Value - Previous;
			Text += FString::Printf(TEXT("  (+%.4f %.4f %.4f)"), D.X, D.Y, D.Z);
			Delta = FLinearColor(D.X, D.Y, D.Z).GetLuminance();
			if (Stage.Index == MOONTOON_DEBUG_STAGE_DirectLights)
			{
				Direct = D;
				bHaveDirect = true;
			}
		}
		FMoonToonDebugField& Field = Out.Stages.AddDefaulted_GetRef();
		Field.Section = TEXT("stages");
		Field.Name = Stage.Name;
		Field.Value = Text;
		Field.Number = Delta;
		Field.Swatch = FLinearColor(Value.X, Value.Y, Value.Z);
		if (!bDisplay)
		{
			Previous = Value;
			bHavePrevious = true;
		}
	}

	// --- main light (trace) ---------------------------------------------------------------------------------------
	Out.MainLightName = R.LightName;
	if (R.bLightValid && R.Trace.Num() == MOONTOON_TRACE_COUNT)
	{
		for (const MoonToonDebug::FFieldInfo& Info : MoonToonDebug::GetTraceInfos())
		{
			const bool bWritten = Info.Index >= MOONTOON_TRACE_EXPOSURE || (R.TraceWrittenMask & (uint64(1) << Info.Index)) != 0u;
			const FVector4f& V = R.Trace[Info.Index];
			FMoonToonDebugField& Field = Out.Trace.AddDefaulted_GetRef();
			Field.Section = TEXT("main_light");
			Field.Name = Info.Name.ToLower();
			const bool bVector = Info.Encoding == MOONTOON_DEBUG_ENC_COLOR || Info.Encoding == MOONTOON_DEBUG_ENC_VECTOR;
			Field.Value = !bWritten ? FString(TEXT("(not reached)")) : (bVector ? Fmt3(V) : FString::Printf(TEXT("%.4f"), V.X));
			if (Info.Index == MOONTOON_TRACE_EXPOSURE)
			{
				Field.Value = FString::Printf(TEXT("toon %.4f, pre-exposure %.4f"), V.X, V.Y);
			}
			else if (Info.Index == MOONTOON_TRACE_PIXEL_CHANNELS)
			{
				Field.Value = FString::Printf(TEXT("pixel 0x%02X, main-light channels 0x%02X, moon pixel %d"), (uint32)V.X, (uint32)V.Y, (int32)V.Z);
			}
			Field.Number = V.X;
			if (bVector && bWritten && Info.Encoding == MOONTOON_DEBUG_ENC_COLOR)
			{
				Field.Swatch = FLinearColor(V.X, V.Y, V.Z);
			}
		}
		if (bHaveDirect)
		{
			const FVector4f& Modelled = R.Trace[MOONTOON_TRACE_SCENE_COLOR_MODELLED];
			const float Measured = FLinearColor(Direct.X, Direct.Y, Direct.Z).GetLuminance();
			const float Model = FLinearColor(Modelled.X, Modelled.Y, Modelled.Z).GetLuminance();
			Out.MainLightAgreement = Measured > 1e-6f ? Model / Measured : 0.0f;
		}
	}

	Out.Json = ToJson(Out);
	return Out;
}

// -------------------------------------------------------------------------------------------------
// JSON

static FString MoonToonJsonToString(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

static TSharedRef<FJsonValue> Pair(FIntPoint P)
{
	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y) });
}

static TSharedRef<FJsonObject> FieldsToJson(const TArray<FMoonToonDebugField>& Fields)
{
	TSharedRef<FJsonObject> Sections = MakeShared<FJsonObject>();
	for (const FMoonToonDebugField& Field : Fields)
	{
		TSharedPtr<FJsonObject> Section = Sections->HasField(Field.Section) ? Sections->GetObjectField(Field.Section) : nullptr;
		if (!Section.IsValid())
		{
			Section = MakeShared<FJsonObject>();
			Sections->SetObjectField(Field.Section, Section);
		}
		if (Field.Source.IsEmpty())
		{
			Section->SetStringField(Field.Name, Field.Value);
		}
		else
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("value"), Field.Value);
			Entry->SetStringField(TEXT("source"), Field.Source);
			Section->SetObjectField(Field.Name, Entry);
		}
	}
	return Sections;
}

FString UMoonToonDebugSubsystem::ToJson(const FMoonToonPixelReport& In)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), In.bValid);
	if (!In.Error.IsEmpty())
	{
		Root->SetStringField(TEXT("error"), In.Error);
	}
	if (!In.bValid)
	{
		return MoonToonJsonToString(Root);
	}
	Root->SetNumberField(TEXT("frame"), (double)In.FrameNumber);
	TSharedRef<FJsonObject> Pixel = MakeShared<FJsonObject>();
	Pixel->SetField(TEXT("viewport"), Pair(In.ViewportPixel));
	Pixel->SetField(TEXT("render"), Pair(In.RenderPixel));
	Pixel->SetField(TEXT("render_size"), Pair(In.RenderSize));
	Pixel->SetField(TEXT("viewport_size"), Pair(In.ViewportSize));
	Pixel->SetNumberField(TEXT("screen_percentage"), In.ScreenPercentage);
	Pixel->SetStringField(TEXT("requested_space"), In.RequestSpace);
	Root->SetObjectField(TEXT("pixel"), Pixel);
	Root->SetBoolField(TEXT("is_toon"), In.bIsToon);
	TSharedRef<FJsonObject> Actor = MakeShared<FJsonObject>();
	Actor->SetNumberField(TEXT("id"), In.ActorId);
	Actor->SetStringField(TEXT("name"), In.ActorName);
	Actor->SetStringField(TEXT("path"), In.ActorPath);
	Root->SetObjectField(TEXT("actor"), Actor);
	TSharedRef<FJsonObject> Profile = MakeShared<FJsonObject>();
	Profile->SetNumberField(TEXT("id"), In.ProfileId);
	Profile->SetStringField(TEXT("path"), In.ProfilePath);
	Root->SetObjectField(TEXT("profile"), Profile);
	TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
	Material->SetStringField(TEXT("path"), In.MaterialPath);
	Material->SetNumberField(TEXT("slot"), In.MaterialSlot);
	Material->SetStringField(TEXT("component"), In.ComponentPath);
	Root->SetObjectField(TEXT("material"), Material);
	TSharedRef<FJsonObject> Validate = MakeShared<FJsonObject>();
	Validate->SetNumberField(TEXT("bits"), In.ValidateBits);
	TArray<TSharedPtr<FJsonValue>> Names;
	for (const FString& Name : In.ValidateNames)
	{
		Names.Add(MakeShared<FJsonValueString>(Name));
	}
	Validate->SetArrayField(TEXT("names"), Names);
	Root->SetObjectField(TEXT("validate"), Validate);
	Root->SetObjectField(TEXT("fields"), FieldsToJson(In.Fields));
	TSharedRef<FJsonObject> Stages = MakeShared<FJsonObject>();
	for (const FMoonToonDebugField& Stage : In.Stages)
	{
		Stages->SetStringField(Stage.Name, Stage.Value);
	}
	Root->SetObjectField(TEXT("stages"), Stages);
	TSharedRef<FJsonObject> MainLight = MakeShared<FJsonObject>();
	MainLight->SetStringField(TEXT("light"), In.MainLightName);
	MainLight->SetNumberField(TEXT("agreement"), In.MainLightAgreement);
	TSharedRef<FJsonObject> Trace = MakeShared<FJsonObject>();
	for (const FMoonToonDebugField& Field : In.Trace)
	{
		Trace->SetStringField(Field.Name, Field.Value);
	}
	MainLight->SetObjectField(TEXT("trace"), Trace);
	Root->SetObjectField(TEXT("main_light"), MainLight);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const FMoonToonRegionStats& Stats)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), Stats.bValid);
	if (!Stats.Error.IsEmpty())
	{
		Root->SetStringField(TEXT("error"), Stats.Error);
	}
	Root->SetStringField(TEXT("view"), Stats.ViewName);
	Root->SetNumberField(TEXT("overflow_pixels"), Stats.OverflowPixels);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FMoonToonRegionValue& V : Stats.Values)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(V.bPerBit ? TEXT("bit") : TEXT("value"), (double)V.Key);
		Entry->SetStringField(TEXT("name"), V.Name);
		Entry->SetNumberField(TEXT("pixels"), V.PixelCount);
		Entry->SetField(TEXT("bounds_min"), Pair(V.BoundsMin));
		Entry->SetField(TEXT("bounds_max"), Pair(V.BoundsMax));
		Entry->SetField(TEXT("sample_render"), Pair(V.Sample));
		Entry->SetField(TEXT("sample_viewport"), Pair(V.SampleViewport));
		Values.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Root->SetArrayField(TEXT("values"), Values);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const TArray<FMoonToonDebugActorInfo>& Actors)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FMoonToonDebugActorInfo& Info : Actors)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("actor_id"), Info.ActorId);
		Entry->SetStringField(TEXT("label"), Info.ActorLabel);
		Entry->SetStringField(TEXT("path"), Info.ActorPath);
		Entry->SetBoolField(TEXT("has_component"), Info.bHasComponent);
		Entry->SetNumberField(TEXT("pixels"), Info.PixelCount);
		TArray<TSharedPtr<FJsonValue>> Materials;
		for (const FString& Material : Info.Materials)
		{
			Materials.Add(MakeShared<FJsonValueString>(Material));
		}
		Entry->SetArrayField(TEXT("materials"), Materials);
		Entry->SetObjectField(TEXT("actor_table"), FieldsToJson(Info.ActorRows));
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Info.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Entry->SetArrayField(TEXT("warnings"), Warnings);
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("actors"), Out);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const TArray<FMoonToonTableRow>& Rows)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FMoonToonTableRow& Row : Rows)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Row.Key);
		TArray<TSharedPtr<FJsonValue>> Columns;
		for (const FString& Column : Row.Columns)
		{
			Columns.Add(MakeShared<FJsonValueString>(Column));
		}
		Entry->SetArrayField(TEXT("columns"), Columns);
		TArray<TSharedPtr<FJsonValue>> Paths;
		for (const FString& Path : Row.ObjectPaths)
		{
			Paths.Add(MakeShared<FJsonValueString>(Path));
		}
		Entry->SetArrayField(TEXT("objects"), Paths);
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("rows"), Out);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const TArray<FMoonToonValidationIssue>& Issues)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FMoonToonValidationIssue& Issue : Issues)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("severity"), Issue.Severity);
		Entry->SetStringField(TEXT("where"), Issue.Where);
		Entry->SetStringField(TEXT("message"), Issue.Message);
		Entry->SetStringField(TEXT("object"), Issue.ObjectPath);
		if (Issue.SamplePixel.X >= 0)
		{
			Entry->SetField(TEXT("sample_render"), Pair(Issue.SamplePixel));
		}
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("issues"), Out);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const TArray<FMoonToonStyleLayer>& Layers)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FMoonToonStyleLayer& Layer : Layers)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("volume"), Layer.Volume);
		Entry->SetStringField(TEXT("volume_path"), Layer.VolumePath);
		Entry->SetNumberField(TEXT("priority"), Layer.Priority);
		Entry->SetNumberField(TEXT("weight"), Layer.Weight);
		Entry->SetStringField(TEXT("preset"), Layer.PresetPath);
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const FString& Field : Layer.OverriddenFields)
		{
			Fields.Add(MakeShared<FJsonValueString>(Field));
		}
		Entry->SetArrayField(TEXT("overridden"), Fields);
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("layers"), Out);
	return MoonToonJsonToString(Root);
}

FString UMoonToonDebugSubsystem::ToJson(const FMoonToonCaptureInfo& Capture)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("valid"), Capture.bValid);
	if (!Capture.Error.IsEmpty())
	{
		Root->SetStringField(TEXT("error"), Capture.Error);
	}
	Root->SetStringField(TEXT("view"), Capture.ViewName);
	Root->SetStringField(TEXT("raw_exr"), Capture.RawPath);
	Root->SetStringField(TEXT("id_png16"), Capture.IdPath);
	Root->SetStringField(TEXT("false_color_png"), Capture.FalseColorPath);
	Root->SetStringField(TEXT("meta"), Capture.MetaPath);
	Root->SetNumberField(TEXT("width"), Capture.Width);
	Root->SetNumberField(TEXT("height"), Capture.Height);
	return MoonToonJsonToString(Root);
}
