// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Framework/Commands/InputChord.h"
#include "InputCoreTypes.h"
#include "MoonToonToolsUserSettings.generated.h"

/**
 * Per-user, per-project state for the tools panel.
 *
 * Only things whose value is entirely in surviving the editor closing. A pinned parameter list that
 * resets every session is not a feature, and there is nothing here worth putting in source control,
 * which is why this is EditorPerProjectUserSettings rather than a project setting.
 */
UCLASS(config = EditorPerProjectUserSettings)
class MOONTOONEDITOR_API UMoonToonToolsUserSettings : public UObject
{
	GENERATED_BODY()

public:
	static UMoonToonToolsUserSettings& Get();

	/** Parameters pinned to the top of the material list, by name. */
	UPROPERTY(config)
	TArray<FName> FavoriteParameters;

	bool IsFavorite(FName ParameterName) const { return FavoriteParameters.Contains(ParameterName); }

	/** Adds or removes, and writes the ini straight away -- there is no other save point. */
	void ToggleFavorite(FName ParameterName);

	/**
	 * The MoonToon debugger's pick shortcut: inspects the pixel under the mouse in a level viewport while the
	 * debugger panel is open. Not a mouse combination -- Ctrl+Alt+drag is box select, Alt+drag orbits.
	 */
	UPROPERTY(config, EditAnywhere, Category = "MoonToon Debugger")
	FInputChord DebugPickChord = FInputChord(EKeys::I, /*bShift*/ true, /*bCtrl*/ true, /*bAlt*/ false, /*bCmd*/ false);

	/** Where the debugger writes captures; empty = <Project>/Saved/MoonToonDebug/captures. */
	UPROPERTY(config, EditAnywhere, Category = "MoonToon Debugger")
	FString DebugCaptureDirectory;

	/**
	 * The debugger's MCP server port (http://127.0.0.1:<port>/mcp). Started by hand: MoonToon.Debug.Mcp.Start or
	 * the panel's MCP button. Change it here and in the Claude Code entry together.
	 */
	UPROPERTY(config, EditAnywhere, Category = "MoonToon Debugger", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 McpPort = 47651;
};
