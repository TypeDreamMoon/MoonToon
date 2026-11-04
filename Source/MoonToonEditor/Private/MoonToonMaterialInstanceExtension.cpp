// Copyright Dream Moon All Rights Reserved.

#include "MoonToonMaterialInstanceExtension.h"

#include "Editor.h"
#include "Engine/MoonToonProfile.h"
#include "Engine/Texture.h"
#include "MaterialEditor/DEditorParameterValue.h"
#include "MaterialEditor/DEditorScalarParameterValue.h"
#include "MaterialEditor/DEditorStaticSwitchParameterValue.h"
#include "MaterialEditor/DEditorTextureParameterValue.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialInstanceEditorExtensions.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"

#define LOCTEXT_NAMESPACE "MoonToonMaterialInstanceExtension"

namespace
{
	/** The dials a bound UMoonToonProfile owns: the record takes the profile's value, not the instance's. */
	struct FProfileDial
	{
		const TCHAR* Parameter;
		FText (*Value)(const FMoonToonProfileStruct&);
	};

	FText FloatText(float V) { return FText::AsNumber(V, &FNumberFormattingOptions::DefaultNoGrouping()); }

	const FProfileDial ProfileDials[] = {
		{ TEXT("Rim Light Intensity"),                [](const FMoonToonProfileStruct& S) { return FloatText(S.RimLightIntensity); } },
		{ TEXT("Rim Light Width"),                    [](const FMoonToonProfileStruct& S) { return FloatText(S.RimLightWidth); } },
		{ TEXT("Screen Space Hair Shadow Intensity"), [](const FMoonToonProfileStruct& S) { return FloatText(S.ScreenSpaceHairShadowIntensity); } },
		{ TEXT("Disable Self Shadow"),                [](const FMoonToonProfileStruct& S) { return S.bDisableShadowMapShadow ? FText::FromString(TEXT("On")) : FText::FromString(TEXT("Off")); } },
	};

	/** Parameters that only act on a Masked instance (their descriptions say so since R11 A10). */
	const TCHAR* MaskedOnly[] = {
		TEXT("Shadow Opacity"), TEXT("Apply Opacity to Shadow"),
		TEXT("Use FX Dissolve"), TEXT("Use FX Reveal"), TEXT("Use FX Hologram"),
	};

	/**
	 * The instance's effective MoonToon profile: its own override, else the parent chain's, else the root's.
	 * Read from the asset, not the editor proxy: the panel's edits are copied to the asset before the panel is
	 * rebuilt, and the proxy's profile field is not a reliable mirror (it is filled only when the root's nodes
	 * name a profile, which M_MoonToon's do not).
	 */
	UMoonToonProfile* EffectiveProfile(const FMIEInstanceContext& Context)
	{
		const UMaterialInterface* Instance = Context.SourceInstance;
		if (!Instance && Context.EditorInstance)
		{
			Instance = Context.EditorInstance->Parent;
		}
		if (!Instance)
		{
			return nullptr;
		}
		if (UMoonToonProfile* Override = Instance->GetMoonToonProfileOverride_Internal())
		{
			return Override;
		}
		if (Instance->NumMoonToonProfile_Internal() > 0)
		{
			return Instance->GetMoonToonProfile_Internal(0);
		}
		return nullptr;
	}

	bool IsMaskedOnly(const FName Name)
	{
		for (const TCHAR* N : MaskedOnly)
		{
			if (Name == FName(N))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * True when the parameter is actually in use: a switch that is on, a 0/1 "Use ..." toggle whose effective value
	 * is on (inherited or not; they default to off), or any other value the instance overrides ("Apply Opacity to
	 * Shadow" defaults to 1, so its value alone would flag every Opaque instance).
	 */
	bool InUse(UDEditorParameterValue* Parameter, bool bIsOverridden)
	{
		if (const UDEditorStaticSwitchParameterValue* Switch = Cast<UDEditorStaticSwitchParameterValue>(Parameter))
		{
			return Switch->ParameterValue != 0;
		}
		if (const UDEditorScalarParameterValue* Scalar = Cast<UDEditorScalarParameterValue>(Parameter))
		{
			const FString Name = Parameter->ParameterInfo.Name.ToString();
			if (Name.StartsWith(TEXT("Use ")))
			{
				return Scalar->ParameterValue != 0.0f;
			}
		}
		return bIsOverridden;
	}

	/** The warnings for one parameter (shared by the rows and the header count). */
	void CollectMessages(const FMIEInstanceContext& Context, UDEditorParameterValue* Parameter, bool bIsOverridden, const FString& Description,
		TArray<FMIEMessage>& Out)
	{
		if (!Parameter || !Context.SourceInstance)
		{
			return;
		}
		const FName Name = Parameter->ParameterInfo.Name;
		if (IsMaskedOnly(Name) && InUse(Parameter, bIsOverridden) && Context.SourceInstance->GetBlendMode() == BLEND_Opaque)
		{
			Out.Add({ EMIEMessageSeverity::Warning, LOCTEXT("NeedsMasked",
				"需要 Masked: 这个实例是 Opaque, 这个参数不起作用(在 Material Property Overrides 里把 Blend Mode 覆盖为 Masked).\n"
				"Needs Masked: this instance is Opaque, so this has no effect (override Blend Mode to Masked).") });
		}
		if (const UDEditorTextureParameterValue* Texture = Cast<UDEditorTextureParameterValue>(Parameter))
		{
			const bool bLinear = Description.Contains(TEXT("颜色空间: 线性")) || Description.Contains(TEXT("Colour space: linear"));
			if (bLinear && Texture->ParameterValue && Texture->ParameterValue->SRGB)
			{
				Out.Add({ EMIEMessageSeverity::Error, FText::Format(LOCTEXT("NeedsLinear",
					"这张图按线性数据采样, 但 {0} 开着 sRGB(导入时关闭 sRGB).\n"
					"Sampled as linear data, but {0} has sRGB on: re-import it with sRGB off."),
					FText::FromName(Texture->ParameterValue->GetFName())) });
			}
		}
	}

	class FMoonToonMIExtension : public IMaterialInstanceEditorExtension
	{
	public:
		virtual FName GetExtensionName() const override { return TEXT("MoonToon"); }

		virtual void DecorateParameterRow(const FMIEParameterContext& Context, FMIEParameterRowDecoration& Decoration) override
		{
			if (UMoonToonProfile* Profile = EffectiveProfile(Context))
			{
				for (const FProfileDial& Dial : ProfileDials)
				{
					if (Context.ParameterInfo.Name != FName(Dial.Parameter))
					{
						continue;
					}
					const FText ProfileName = FText::FromString(Profile->GetName());
					const FText Value = Dial.Value(Profile->Settings);
					Decoration.NameColor = FSlateColor(FLinearColor(1.0f, 0.72f, 0.25f));
					Decoration.NameFont = FAppStyle::GetFontStyle("PropertyWindow.ItalicFont");
					Decoration.OverriddenReasons.Add(FText::Format(LOCTEXT("OverriddenReason",
						"由 MoonToon Profile '{0}' 覆盖, 生效值 {1}; 去掉 Profile 后这里的值重新生效.\n"
						"Overridden by MoonToon profile '{0}' (value {1}); this value applies again if the profile is removed."),
						ProfileName, Value));
					FMIEBadge Badge;
					Badge.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link");
					Badge.Tint = FSlateColor(FLinearColor(1.0f, 0.72f, 0.25f));
					Badge.ToolTip = FText::Format(LOCTEXT("ProfileBadge", "Profile '{0}': {1}  (点击打开 / click to open)"), ProfileName, Value);
					TWeakObjectPtr<UMoonToonProfile> WeakProfile = Profile;
					Badge.OnClicked = FSimpleDelegate::CreateLambda([WeakProfile]()
					{
						if (UMoonToonProfile* P = WeakProfile.Get(); P && GEditor)
						{
							GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(P);
						}
					});
					Decoration.BadgesAfterValue.Add(Badge);
					break;
				}
			}
			CollectMessages(Context, Context.Parameter, Context.bIsOverridden, Context.Description, Decoration.Messages);
		}

		/** The checks read parameter values, so a committed change to a checked parameter rebuilds the panel. */
		virtual void OnParameterValueChanged(const FMIEInstanceContext& Context, const FMaterialParameterInfo& Changed, bool bInteractive) override
		{
			if (bInteractive || !Context.EditorInstance)
			{
				return;
			}
			bool bChecked = IsMaskedOnly(Changed.Name);
			for (const FEditorParameterGroup& Group : Context.EditorInstance->ParameterGroups)
			{
				for (UDEditorParameterValue* Parameter : Group.Parameters)
				{
					bChecked |= Parameter && Parameter->ParameterInfo == Changed && Parameter->IsA<UDEditorTextureParameterValue>();
				}
			}
			if (bChecked)
			{
				FMaterialInstanceEditorExtensions::Get().RequestRefresh(Context.EditorInstance);
			}
		}

		virtual void BuildHeader(const FMIEInstanceContext& Context, FMIEHeaderBuilder& Header) override
		{
			if (!Context.EditorInstance)
			{
				return;
			}
			if (UMoonToonProfile* Profile = EffectiveProfile(Context))
			{
				Header.AddMessage(EMIEMessageSeverity::Info, FText::Format(LOCTEXT("ProfileHeader",
					"MoonToon Profile '{0}' 决定 Rim Light Intensity / Width、Screen Space Hair Shadow Intensity、Disable Self Shadow(标黄的行).\n"
					"MoonToon profile '{0}' decides Rim Light Intensity / Width, Screen Space Hair Shadow Intensity and Disable Self Shadow (the amber rows)."),
					FText::FromString(Profile->GetName())));
			}
			int32 Warnings = 0;
			for (const FEditorParameterGroup& Group : Context.EditorInstance->ParameterGroups)
			{
				for (UDEditorParameterValue* Parameter : Group.Parameters)
				{
					if (!Parameter || !Context.EditorInstance->VisibleExpressions.Contains(Parameter->ParameterInfo))
					{
						continue;
					}
					TArray<FMIEMessage> Messages;
					CollectMessages(Context, Parameter, Parameter->bOverride, Parameter->Description, Messages);
					Warnings += Messages.Num();
				}
			}
			if (Warnings > 0)
			{
				Header.AddMessage(EMIEMessageSeverity::Warning, FText::Format(LOCTEXT("WarningCount",
					"MoonToon: {0} 个参数有提示, 见各行下方. {0} parameter(s) have a note under their row."), Warnings));
			}
		}
	};

	FMaterialInstanceEditorExtensionHandle GHandle;
}

namespace MoonToonMaterialInstanceExtension
{
	void Register()
	{
		if (!GHandle.IsValid())
		{
			GHandle = FMaterialInstanceEditorExtensions::Get().Register(MakeShared<FMoonToonMIExtension>(), 100);
		}
	}

	void Unregister()
	{
		FMaterialInstanceEditorExtensions::Get().Unregister(GHandle);
	}
}

#undef LOCTEXT_NAMESPACE
