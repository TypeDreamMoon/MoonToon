// Copyright Dream Moon All Rights Reserved.

#pragma once

/**
 * R11 T57c: MoonToon's client of the material instance editor extensions (engine MaterialEditor,
 * MaterialInstanceEditorExtensions.h; Docs/MaterialInstanceEditorExtensions.md):
 *  - the parameters a bound UMoonToonProfile overrides (Rim Light Intensity / Width, Screen Space Hair Shadow
 *    Intensity, Disable Self Shadow) are drawn amber and italic, dimmed but editable, with a profile badge whose
 *    tooltip gives the profile's value and which opens the profile when clicked;
 *  - inline warnings: a Masked-only parameter in use on an Opaque instance, a linearly sampled map holding an
 *    sRGB texture;
 *  - a header line naming the bound profile and counting the warnings.
 */
namespace MoonToonMaterialInstanceExtension
{
	void Register();
	void Unregister();
}
