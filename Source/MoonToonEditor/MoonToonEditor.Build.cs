// Copyright Dream Moon. All Rights Reserved.

using UnrealBuildTool;

public class MoonToonEditor : ModuleRules
{
	public MoonToonEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"UnrealEd",
				// UEditorUtilityLibrary::GetSelectedAssets, and the UAssetActionUtility base class.
				"Blutility",
				// The Scripted Asset Actions menu is driven by the asset registry and only ever finds
				// Blueprint assets, so a native tool has to register its own context-menu entries.
				"ToolMenus",
				// FUIAction / FSlateIcon for the menu entries.
				"Slate",
				"SlateCore",
				// EKeys, referenced by the SListView template instantiations in the tools panel.
				"InputCore",
				// LOD build settings get/set, mirroring what the original Blueprint called.
				"StaticMeshEditor",
				"SkeletalMeshEditor",
				// Welding / split-normal computation / per-vertex normals / nearest-point queries.
				"GeometryScriptingCore",
				"GeometryFramework",
				// FRawMesh, for static-mesh face material indices the mesh-data library does not expose.
				"RawMesh",
				// FRenderCommandFence, for the release-patch-reinit dance in PatchVertexAlphaLive.
				"RenderCore",
				// GMaxRHIShaderPlatform -- the default argument of FMaterialUpdateContext's constructor,
				// so every re-parent site needs it even though none of them names a shader platform.
				"RHI",
				// The tools panel: IDetailsView renders each tool's UPROPERTYs, so no tool writes Slate.
				"PropertyEditor",
				// UMaterialEditingLibrary: the supported way to write a material instance and publish
				// the change (dirty, rebuild, static permutation, viewport redraw).
				"MaterialEditor",
				// Nomad tab registration under the Window > Tools category.
				"WorkspaceMenuStructure",
				// Following the Content Browser selection, and syncing it to a section's material.
				"ContentBrowser",
				// FPlatformApplicationMisc::ClipboardCopy, behind the panel's copy buttons.
				"ApplicationCore",
				// OpenColorPicker, for the vector parameters in the material editor pane.
				"AppFramework",
				// Mesh import-data read/write, MikkTSpace tangents, curvature. Lives in the engine
				// plugin MoonToonScripts, which is EnabledByDefault.
				"MoonToonEditorScripts",
			}
		);
	}
}
