# Viewmodel FOV

A PAYDAY 2 mod that sets the FOV of your gun and arms separately from the world FOV.

This is a real change of perspective, not a position offset, so it works alongside position mods such as Perfect View Model.

## Features

- Separate FOV for your gun and arms, without changing the world FOV
- Optionally keeps the gun FOV while aiming down sights
- Gun lights, charm glows, muzzle flashes and shell ejection are moved to match the gun
- Masks and the mask-on animation are drawn as in vanilla, easing back to the gun FOV afterwards
- Changes apply straight away

## Requirements

- [SuperBLT](https://superblt.znix.xyz/), which is needed to load the native plugin
- Windows, 64-bit PAYDAY 2

## Installation

1. Download `Viewmodel-FOV-v1.0.zip` from the [Releases](../../releases) page.
2. Extract it into your `PAYDAY 2/mods/` folder. The result should be `PAYDAY 2/mods/Viewmodel FOV/mod.txt`.
3. Start the game.

The release includes the compiled plugin, `native/vmfov_native.dll`. Cloning the repository does not give you the DLL. See [Building](#building) to make it yourself.

## Settings

Find these under **Options > Mod Options > Viewmodel FOV**.

| Setting | Description |
| --- | --- |
| Enabled | Turns the mod on or off |
| Gun FOV | The FOV your gun and arms are drawn at (40 to 120). Lower makes them larger and closer |
| Apply While Aiming | Keeps the gun FOV while aiming down sights. Leave it off to keep the normal sight picture |

Settings are saved to `mods/saves/ViewmodelFOV.json`.

## Compatibility

- **Perfect View Model** and other viewmodel position mods work alongside it.
- **Masks** are left untouched. The gun FOV turns off for the mask-on animation.
- **Vehicles**: the gun FOV turns off while driving.
- **Overlays and graphics injectors** that also hook Direct3D 11 (for example ReShade) have not been tested together with this mod.
- **VR** is not supported.

## Known limitations

- **Gun FOV only works below your world FOV.** Higher values have no effect.
- Muzzle flashes line up with the gun, but they do not grow with it, so they can look a little small at low Gun FOV values.
- Laser beams are not adjusted, so they may not line up exactly when firing from the hip.
- Tested with a limited set of weapons and charms. A charm built differently could still have a displaced glow.

## How it works

The game draws the gun and the world with one camera, so there is no second FOV to set. It does draw the gun in its own stage, though. A native plugin hooks the game's Direct3D 11 draw calls, finds that stage, and scales the viewport for those draws about the centre of the screen. That has the same effect as drawing them with a different FOV. Glow layers drawn later are matched to the gun. Lights on the gun, and the points that muzzle flashes and shells spawn from, are moved to where the gun appears. Bullets are fired from the camera, so aiming is not affected.

This only changes what is drawn on your own screen.

## Building

The plugin source is in `native/src`. You need Visual Studio with the **Desktop development with C++** workload. Run:

```
native\build.bat
```

This produces `native/vmfov_native.dll`. Close the game first, since the DLL is locked while PAYDAY 2 is running.

Some antivirus programs can be wary of DLLs that hook a game's renderer, which is a good reason to build it yourself if you prefer.

## Licence

The native plugin is licensed under the GNU GPL, as SuperBLT requires. See `native/LICENSE.txt`.
