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
				// The tools panel: IDetailsView renders each tool's UPROPERTYs, so no tool writes Slate.
				"PropertyEditor",
				// Nomad tab registration under the Window > Tools category.
				"WorkspaceMenuStructure",
				// Following the Content Browser selection.
				"ContentBrowser",
				// Mesh import-data read/write, MikkTSpace tangents, curvature. Lives in the engine
				// plugin MoonToonScripts, which is EnabledByDefault.
				"MoonToonEditorScripts",
			}
		);
	}
}
