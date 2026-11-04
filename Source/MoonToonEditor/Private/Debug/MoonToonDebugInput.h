// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

class UMoonToonDebugSubsystem;

/**
 * Picking a pixel in a level viewport for the MoonToon debugger, ahead of the viewport's own input:
 *   - pick mode (the panel's button): the next left click over a level viewport inspects that pixel and is
 *     consumed, so it does not change the selection; Esc leaves pick mode;
 *   - the pick shortcut (UMoonToonToolsUserSettings::DebugPickChord, Ctrl+Shift+I by default) inspects the
 *     pixel under the cursor, while the debugger panel is open.
 * Mouse combinations are avoided on purpose: Ctrl+Alt+drag is the viewport's box select, Alt+drag orbits.
 */
class FMoonToonDebugInputProcessor : public IInputProcessor
{
public:
	explicit FMoonToonDebugInputProcessor(UMoonToonDebugSubsystem* InSubsystem) : Subsystem(InSubsystem) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("MoonToonDebugPick"); }

private:
	TWeakObjectPtr<UMoonToonDebugSubsystem> Subsystem;
};
