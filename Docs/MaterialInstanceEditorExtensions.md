# Material instance editor extensions (材质实例编辑器扩展)

The Moon engine's material instance editor has one extension point: a registry in the `MaterialEditor`
module through which any editor module can decorate the parameter panel without touching engine code.
MoonToon is its first client; the API is generic and knows nothing about toon shading.

- Engine header: `Engine/Source/Editor/MaterialEditor/Public/MaterialInstanceEditorExtensions.h`
- Engine implementation: `Engine/Source/Editor/MaterialEditor/Private/MaterialInstanceEditorExtensions.cpp`
  (hooks in `MaterialEditorInstanceDetailCustomization.cpp` and `MaterialInstanceEditor.cpp`)
- MoonToon client: `Source/MoonToonEditor/Private/MoonToonMaterialInstanceExtension.cpp`

**Nothing registered = no change.** Every engine hook is behind `FMaterialInstanceEditorExtensions::HasExtensions()`;
with no extension the panel is built exactly as in stock UE.

## What an extension can do

| Hook | When | What it can change |
| --- | --- | --- |
| `DecorateParameterRow` | per parameter row, while the panel is built | name colour / font, badges (before the name, before / after the value; clickable or not), tooltip lines, an *overridden* state with reasons (dimmed, still editable), a *read-only* state with reasons, messages under the row (info / warning / error), reset-to-default behaviour, context-menu entries |
| `ShouldShowParameter` | per parameter row | hide the row (`false`); unset leaves the editor's own rule |
| `DecorateParameterGroup` | per group header (`A\|B` groups: one call per level, full path) | header badges, header colour, start expanded / collapsed, header context-menu entries |
| `BuildHeader` | once per panel build | rows at the top of the *Parameter Groups* category: messages or any widget |
| `OnParameterValueChanged` | a parameter value or its override checkbox changed (`bInteractive` during a slider drag) | react; call `RequestRefresh` if your decorations depend on it |
| `OnInstancePropertyChanged` | any other instance property changed (`Parent`, `MoonToonProfile`, `BasePropertyOverrides`, ...) | react; the panel is rebuilt after this anyway |

`FMaterialInstanceEditorExtensions::RequestRefresh(EditorInstance)` rebuilds the panel of one open instance
editor (or all of them with `nullptr`). Register / Unregister refresh every open editor.

## Rules

- **Game thread only.** Decorate hooks run while the details panel is built: keep them cheap, and never modify
  the instance from them. Change state from the Notify hooks or your own UI, then `RequestRefresh`.
- **Order:** ascending priority, then registration order. Single-valued fields (name colour, name font,
  reset-to-default override, a group's colour / expansion) come from the **last** extension that sets them, so
  the highest priority wins. Lists (badges, tooltip lines, messages, menu entries) are concatenated.
  *Overridden* and *read-only* are OR-ed and every reason is listed in the tooltip.
- **Hiding wins:** if any extension returns `false` from `ShouldShowParameter`, the row is hidden.
- **Overridden vs read-only:** *overridden* means something outside the instance decides the value (a bound
  profile, a runtime system); the row is drawn dimmed but stays editable, because its value applies again when
  the override goes away. *Read-only* disables the row; use it only when editing it is actually wrong.
- **Messages** are drawn as their own rows under the parameter, and follow its visibility (hidden parameters
  hide their messages too).
- **Not covered (v1):** the material layers tree (`Layer Parameters` tab) and material function instances.

## Minimal example

```cpp
#include "MaterialInstanceEditorExtensions.h"

class FMyExtension : public IMaterialInstanceEditorExtension
{
public:
	virtual FName GetExtensionName() const override { return TEXT("MyPlugin"); }

	virtual void DecorateParameterRow(const FMIEParameterContext& Context, FMIEParameterRowDecoration& Decoration) override
	{
		if (Context.ParameterInfo.Name == TEXT("Wind Strength") && IsDrivenByWindSystem(Context.SourceInstance))
		{
			Decoration.NameColor = FSlateColor(FLinearColor(0.4f, 0.8f, 1.0f));
			Decoration.OverriddenReasons.Add(INVTEXT("Driven by the wind system at runtime."));
		}
	}
};

// Editor module:
FMaterialInstanceEditorExtensionHandle Handle;
void FMyEditorModule::StartupModule()  { Handle = FMaterialInstanceEditorExtensions::Get().Register(MakeShared<FMyExtension>(), 50); }
void FMyEditorModule::ShutdownModule() { FMaterialInstanceEditorExtensions::Get().Unregister(Handle); }
```

The module needs `MaterialEditor` in its private dependencies (editor modules only; the header pulls in no
PropertyEditor or Slate widget headers, only `FSlateIcon`, `FSlateColor`, `FSlateFontInfo`, `FUIAction`).

## What MoonToon registers (priority 100)

1. **Profile-owned parameters.** When the instance has an effective `UMoonToonProfile` (its own override, else
   the parent chain's, else the root material's first profile), the four parameters the profile replaces in the
   shading record are marked:
   `Rim Light Intensity`, `Rim Light Width`, `Screen Space Hair Shadow Intensity`, `Disable Self Shadow`.
   Name in amber italics, row dimmed but editable, a link badge after the value whose tooltip gives the
   profile's value (click opens the profile), and the reason in the row tooltip.
2. **Masked-only parameters on an Opaque instance.** `Shadow Opacity`, `Apply Opacity to Shadow`,
   `Use FX Dissolve`, `Use FX Reveal`, `Use FX Hologram`: a warning under the row when the parameter is in use
   and the instance's blend mode is Opaque. In use = a switch that is on, a `Use ...` 0/1 toggle whose effective
   value is on, or any other parameter the instance overrides (`Apply Opacity to Shadow` defaults to 1, so its
   value alone is not a signal).
3. **Linear maps holding an sRGB texture.** A texture parameter whose description says
   `颜色空间: 线性` / `Colour space: linear` and whose texture has sRGB on gets an error under the row.
4. **Header.** An info line naming the effective profile and the rows it decides, and a count of the rows
   that carry a warning or error.
5. **The profile is read from the asset**, not the editor proxy: `UMaterialInstance::GetMoonToonProfileOverride_Internal()`
   on the source instance, else the root's first profile.

To add a MoonToon check, extend `CollectMessages` in the client (it is shared by the rows and the header count).
Parameter names are the identity here: when a parameter is renamed (see `Tools/Migration`), update the tables
at the top of the client too.

## Design notes

- The registry lives in the engine's `MaterialEditor` module rather than in the plugin because the detail
  customization that builds the rows is private to that module; a plugin cannot reach into it otherwise.
- Rows are decorated after the stock row is built (`SetPropertyRowParameterWidget`), so the stock name,
  value widget, override checkbox and reset button are kept; an extension only wraps or recolours them.
- The panel is rebuilt (not patched) whenever an instance property changes, so decorations never go stale
  against the parent / profile; value changes do not rebuild unless an extension asks, to keep slider drags
  smooth.
