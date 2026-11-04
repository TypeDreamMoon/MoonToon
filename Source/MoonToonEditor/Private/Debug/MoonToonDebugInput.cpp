// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonDebugInput.h"

#include "MoonToonDebugSubsystem.h"
#include "MoonToonToolsUserSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "LevelEditorViewport.h"

bool FMoonToonDebugInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub)
	{
		return false;
	}
	if (Sub->IsPickArmed() && InKeyEvent.GetKey() == EKeys::Escape)
	{
		Sub->SetPickArmed(false);
		return true;
	}
	if (!Sub->IsPanelOpen())
	{
		return false;
	}
	const FInputChord& Chord = UMoonToonToolsUserSettings::Get().DebugPickChord;
	if (!Chord.IsValidChord() || InKeyEvent.GetKey() != Chord.Key
		|| InKeyEvent.IsControlDown() != Chord.bCtrl || InKeyEvent.IsShiftDown() != Chord.bShift
		|| InKeyEvent.IsAltDown() != Chord.bAlt || InKeyEvent.IsCommandDown() != Chord.bCmd)
	{
		return false;
	}
	FLevelEditorViewportClient* Client = nullptr;
	FIntPoint Pixel;
	if (!Sub->FindViewportUnderCursor(Client, Pixel))
	{
		return false;
	}
	Sub->PickAt(Client, Pixel);
	return true;
}

bool FMoonToonDebugInputProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub || !Sub->IsPickArmed() || MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return false;
	}
	FLevelEditorViewportClient* Client = nullptr;
	FIntPoint Pixel;
	if (!Sub->FindViewportUnderCursor(Client, Pixel))
	{
		return false;	// a click elsewhere (the panel itself) keeps pick mode on
	}
	Sub->SetPickArmed(false);
	Sub->PickAt(Client, Pixel);
	return true;	// consumed: picking does not select
}
