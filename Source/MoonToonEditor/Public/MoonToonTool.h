// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MoonToonTool.generated.h"

/** What the panel hands a tool when Run is pressed. */
struct FMoonToonToolContext
{
	/** Meshes the panel currently targets. Static or skeletal; never null, never other classes. */
	TArray<TWeakObjectPtr<UObject>> Meshes;

	/** Material indices to restrict to. Empty means every section. */
	TArray<int32> SectionMaterialIndices;
};

/**
 * One entry in the MoonToon tools panel.
 *
 * A tool is a plain UObject: its UPROPERTYs are the settings UI (the panel renders them with an
 * IDetailsView, so no tool writes Slate), and Run does the work. Adding a tool to the panel is
 * therefore a matter of declaring properties and implementing Run -- nothing else in the panel
 * changes.
 *
 * Run returns a report string rather than logging, because the panel shows it in an output pane
 * where it can actually be read; the read-only tools (mesh info) are almost entirely report.
 */
UCLASS(Abstract)
class MOONTOONEDITOR_API UMoonToonTool : public UObject
{
	GENERATED_BODY()

public:
	/** Label in the tool list. */
	virtual FText GetToolName() const { return FText::FromName(GetClass()->GetFName()); }

	/** Tooltip in the tool list, and the blurb above the settings. */
	virtual FText GetToolDescription() const { return FText::GetEmpty(); }

	/** Label on the action button. */
	virtual FText GetRunLabel() const;

	/** True when the tool writes to the asset, which the panel reflects in the button styling. */
	virtual bool IsDestructive() const { return true; }

	/**
	 * True when running the tool can change the mesh's section layout -- which in practice means it
	 * can trigger a reimport.
	 *
	 * The panel rebuilds its section list only for these. Rebuilding is not free: it re-reads the
	 * mesh import data and re-applies the viewport highlight, and re-applying the highlight reattaches
	 * the component.
	 */
	virtual bool InvalidatesSectionList() const { return false; }

	/** Does the work. Returns a human-readable report for the panel's output pane. */
	virtual FString Run(const FMoonToonToolContext& Context)
	{
		return FString();
	}

	/**
	 * Script entry point: the same work the panel's Run button does, with the target supplied
	 * explicitly rather than read from the editor selection.
	 *
	 * Exists so a tool can be driven from Python -- batch runs across many meshes, or a regression
	 * check that asserts on the resulting vertex data. The panel and Python go through one
	 * implementation; there is no scripting-only code path to drift.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon")
	FString RunOnMeshes(const TArray<UObject*>& Meshes, const TArray<int32>& SectionMaterialIndices);

	/**
	 * Bumped whenever something OUTSIDE the panel writes this tool's properties -- currently the
	 * strand live-preview actor pushing the ellipsoid it was just dragged to. The panel polls it and
	 * repaints the details view, which otherwise keeps showing the values from before the drag.
	 */
	UPROPERTY(Transient)
	uint32 ExternalEditSerial = 0;

protected:
	/**
	 * Shared preamble: resolves the context to live meshes and reports the ones that went away.
	 * Returns false when there is nothing to operate on, with OutReport already explaining why.
	 */
	static bool ResolveMeshes(const FMoonToonToolContext& Context, TArray<UObject*>& OutMeshes, FString& OutReport);
};
