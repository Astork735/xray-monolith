<html><body>
<!--StartFragment--><html><head></head><body><h1><span>X-Ray Monolith VR</span></h1><p><strong><span>Experimental native OpenXR VR fork of </span></strong><a href="https://github.com/themrdemonized/xray-monolith"><strong><span>S.T.A.L.K.E.R. Anomaly Modded Exes / X-Ray Monolith</span></strong></a><strong><span>.</span></strong></p><blockquote><p><strong><span>Alpha / Work in Progress:</span></strong><span> This is an experimental VR project, not a stable release. Back up your saves. Full desktop-to-VR and VR-to-desktop save compatibility should work though there may be some issues.</span></p></blockquote><h2><span>About the Project</span></h2><p><span>X-Ray Monolith VR aims to bring immersive, physically interactive virtual reality gameplay to S.T.A.L.K.E.R. Anomaly, while preserving the existing X-Ray engine and its modding capabilities.</span></p><p><span>The project introduces native OpenXR support, tracked controllers, physical weapon handling, VR body interactions, and controller-based access to existing game interfaces.</span></p><h3><span>Project Origins &amp; Credits</span></h3><p><span>This project is forked from </span><a href="https://github.com/themrdemonized/xray-monolith"><strong><span>themrdemonized/xray-monolith</span></strong></a><span>, also known as S.T.A.L.K.E.R. Anomaly Modded Exes.</span></p><p><span>The upstream project contains extensive engine improvements and community contributions. Full credit for that work belongs to its original authors and contributors.</span></p><p><span>This fork focuses on developing the VR functionality and associated gameplay systems.</span></p><h2><span>Features</span></h2><h3><span>Native OpenXR Support</span></h3><ul><li><p><span>Native stereoscopic VR rendering.</span></p></li><li><p><span>Headset and controller tracking.</span></p></li><li><p><span>Compatible PCVR OpenXR runtime support.</span></p></li><li><p><span>Room-scale movement and physical crouching.</span></p></li><li><p><span>Snap and smooth turning.</span></p></li><li><p><span>Automatic desktop fallback if VR is unavailable.</span></p></li></ul><h3><span>Physical Weapons</span></h3><ul><li><p><span>Grab, hold, fire, drop and holster weapons.</span></p></li><li><p><span>Two-handed aiming and handling.</span></p></li><li><p><span>Manual magazine insertion and removal.</span></p></li><li><p><span>Physical bolt, slide and charging-handle interactions.</span></p></li><li><p><span>Shell-by-shell shotgun loading.</span></p></li><li><p><span>Break-action and belt-fed weapon interactions.</span></p></li><li><p><span>Weapon mechanics derived from existing models and animations.</span></p></li><li><p><span>Native reload fallback for unsupported weapons.</span></p></li></ul><h3><span>VR Body &amp; Equipment</span></h3><ul><li><p><span>Experimental full-body VR representation and IK.</span></p></li><li><p><span>Body-mounted weapon holsters.</span></p></li><li><p><span>Physical equipment slots.</span></p></li><li><p><span>Grenade and bolt throwing.</span></p></li><li><p><span>Inventory storage through physical gestures.</span></p></li><li><p><span>Flashlight, detector, PDA and night-vision interactions.</span></p></li></ul><h3><span>VR Interface</span></h3><ul><li><p><span>Controller-driven inventory, PDA, dialogue, trade and loot screens.</span></p></li><li><p><span>Point-and-click UI interaction.</span></p></li><li><p><span>Dragging and scrolling.</span></p></li><li><p><span>VR virtual keyboard.</span></p></li><li><p><span>VR configuration through MCM and console settings.</span></p></li></ul><h3><span>VR Optics</span></h3><ul><li><p><span>Experimental magnified scopes.</span></p></li><li><p><span>Red dot and reflex sights.</span></p></li><li><p><span>Configurable scope rendering options.</span></p></li></ul><h3><span>Experimental Blood System</span></h3><ul><li><p><span>Blood sprays and surface decals.</span></p></li><li><p><span>Blood pooling and flow effects.</span></p></li><li><p><span>Adjustable effects and performance settings.</span></p></li><li><p><span>Optional MCM-controlled configuration.</span></p></li></ul><h2><span>Requirements</span></h2><ul><li><p><span>Windows PC capable of running S.T.A.L.K.E.R. Anomaly.</span></p></li><li><p><span>Compatible PCVR headset, such as Meta Quest 3.</span></p></li><li><p><span>OpenXR-compatible PCVR runtime.</span></p></li><li><p><span>X-Ray Monolith VR DX11 executable.</span></p></li><li><p><span>OpenXR loader DLL.</span></p></li><li><p><span>Required repository gamedata content.</span></p></li><li><p><span>Anomaly MCM for the optional VR settings module.</span></p></li></ul><h2><span>Getting Started</span></h2><ol start="1"><li><p><span>Prepare a compatible S.T.A.L.K.E.R. Anomaly installation.</span></p></li><li><p><span>Build the DX11 x64 engine from this repository using Visual Studio 2022, or obtain a compiled build if one is provided.</span></p></li><li><p><span>Install the required </span><code><span>gamedata</span></code><span> files.</span></p></li><li><p><span>Place </span><code><span>openxr_loader.dll</span></code><span> alongside </span><code><span>AnomalyDX11.exe</span></code><span>.</span></p></li><li><p><span>Install </span><code><span>VR_Settings_MCM</span></code><span> if using the MCM menu.</span></p></li><li><p><span>Connect your headset using a compatible PCVR runtime.</span></p></li><li><p><span>Launch the game.</span></p></li></ol><h3><span>Launch Options</span></h3>
Command | Behavior
-- | --
AnomalyDX11.exe | Automatic VR detection
AnomalyDX11.exe -vr | Request VR mode
AnomalyDX11.exe -novr | Force desktop mode

<p><strong><span>Important:</span></strong><span> Weapons remain held only while the grip is pressed. Physical reloading varies by weapon type.</span></p><p><span>For detailed controls, see </span><a><span>docs/VR.md</span></a><span>.</span></p><h2><span>Known Issues</span></h2><ul><li><p><span>Some weapons have incorrect reload behavior or animations.</span></p></li><li><p><span>GAMMA weapon alignment and mechanics remain inconsistent.</span></p></li><li><p><span>Some physical reload interactions can become stuck.</span></p></li><li><p><span>Certain shotguns may have unresolved jam-recovery problems.</span></p></li><li><p><span>Scope and binocular support is incomplete.</span></p></li><li><p><span>VR body and equipment slots may shift unexpectedly.</span></p></li><li><p><span>Some modded script actions cannot be accessed through VR controllers.</span></p></li><li><p><span>Left-handed and seated support require more testing.</span></p></li><li><p><span>Full desktop/VR save interchangeability is unverified.</span></p></li><li><p><span>Blood effects and desktop rendering require further regression testing.</span></p></li></ul><h2><span>Development Status</span></h2><p><span>This is an experimental alpha.</span></p><p><span>The November 1, 2026 audit recorded ten passing automated VR test suites, but did not establish a clean final binary or complete in-headset, desktop, and cross-mode save acceptance.</span></p><p><span>Features are being developed and tested incrementally. Not every supported system has been fully validated in-game.</span></p><h2><span>Bug Reports &amp; Contributions</span></h2><p><span>Issues, bug reports, compatibility feedback, and contributions are welcome.</span></p><p><span>When reporting a problem, please include:</span></p><ul><li><p><span>Headset and controller type.</span></p></li><li><p><span>OpenXR runtime.</span></p></li><li><p><span>Game and modpack configuration.</span></p></li><li><p><span>Build or commit version.</span></p></li><li><p><span>Steps to reproduce.</span></p></li><li><p><span>Relevant logs or screenshots.</span></p></li></ul><h2><span>Links &amp; Acknowledgements</span></h2><p><strong><span>VR fork:</span></strong><span> </span><a href="https://github.com/Astork735/xray-monolith"><span>Astork735/xray-monolith</span></a></p><p><strong><span>Original upstream project:</span></strong><span> </span><a href="https://github.com/themrdemonized/xray-monolith"><span>themrdemonized/xray-monolith</span></a></p><p><strong><span>Releases:</span></strong><span> </span><a href="https://github.com/Astork735/xray-monolith/releases"><span>X-Ray Monolith VR Releases</span></a></p><p><span>This project builds on the work of the original X-Ray engine developers, the Anomaly modding community, themrdemonized, and the many upstream contributors.</span></p></body></html><!--EndFragment-->
</body>
</html># X-Ray Monolith VR

**Experimental native OpenXR VR fork of [[S.T.A.L.K.E.R. Anomaly Modded Exes / X-Ray Monolith](https://github.com/themrdemonized/xray-monolith)](https://github.com/themrdemonized/xray-monolith).**

> **Alpha / Work in Progress:** This is an experimental VR project, not a stable release. Back up your saves. Full desktop-to-VR and VR-to-desktop save compatibility has not yet been verified.

## About the Project

X-Ray Monolith VR aims to bring immersive, physically interactive virtual reality gameplay to S.T.A.L.K.E.R. Anomaly, while preserving the existing X-Ray engine and its modding capabilities.

The project introduces native OpenXR support, tracked controllers, physical weapon handling, VR body interactions, and controller-based access to existing game interfaces.

### Project Origins & Credits

This project is forked from **[[themrdemonized/xray-monolith](https://github.com/themrdemonized/xray-monolith)](https://github.com/themrdemonized/xray-monolith)**, also known as S.T.A.L.K.E.R. Anomaly Modded Exes.

The upstream project contains extensive engine improvements and community contributions. Full credit for that work belongs to its original authors and contributors.

This fork focuses on developing the VR functionality and associated gameplay systems.

The underlying X-Ray engine has non-commercial-use restrictions. All applicable licenses and attribution must be preserved.

## Features

### Native OpenXR Support

- Native stereoscopic VR rendering.
- Headset and controller tracking.
- Compatible PCVR OpenXR runtime support.
- Room-scale movement and physical crouching.
- Snap and smooth turning.
- Automatic desktop fallback if VR is unavailable.

### Physical Weapons

- Grab, hold, fire, drop and holster weapons.
- Two-handed aiming and handling.
- Manual magazine insertion and removal.
- Physical bolt, slide and charging-handle interactions.
- Shell-by-shell shotgun loading.
- Break-action and belt-fed weapon interactions.
- Weapon mechanics derived from existing models and animations.
- Native reload fallback for unsupported weapons.

### VR Body & Equipment

- Experimental full-body VR representation and IK.
- Body-mounted weapon holsters.
- Physical equipment slots.
- Grenade and bolt throwing.
- Inventory storage through physical gestures.
- Flashlight, detector, PDA and night-vision interactions.

### VR Interface

- Controller-driven inventory, PDA, dialogue, trade and loot screens.
- Point-and-click UI interaction.
- Dragging and scrolling.
- VR virtual keyboard.
- VR configuration through MCM and console settings.

### VR Optics

- Experimental magnified scopes.
- Red dot and reflex sights.
- Configurable scope rendering options.

### Experimental Blood System

- Blood sprays and surface decals.
- Blood pooling and flow effects.
- Adjustable effects and performance settings.
- Optional MCM-controlled configuration.

## Requirements

- Windows PC capable of running S.T.A.L.K.E.R. Anomaly.
- Compatible PCVR headset, such as Meta Quest 3.
- OpenXR-compatible PCVR runtime.
- X-Ray Monolith VR DX11 executable.
- OpenXR loader DLL.
- Required repository gamedata content.
- Anomaly MCM for the optional VR settings module.

## Getting Started

1. Prepare a compatible S.T.A.L.K.E.R. Anomaly installation.
2. Build the DX11 x64 engine from this repository using Visual Studio 2022, or obtain a compiled build if one is provided.
3. Install the required `gamedata` files. (Use mod organizer or JGSME
4. Place `openxr_loader.dll` alongside `AnomalyDX11.exe`.
5. Install `VR_Settings_MCM` if using the MCM menu.
6. Connect your headset using a compatible PCVR runtime.
7. Launch the game.

### Launch Options

| Command | Behavior |
|---|---|
| `AnomalyDX11.exe` | Automatic VR detection |
| `AnomalyDX11.exe -vr` | Request VR mode |
| `AnomalyDX11.exe -novr` | Force desktop mode |

## Basic Quest 3 Controls

Default right-handed Touch Plus layout.

| Control | Action |
|---|---|
| Left joystick | Move |
| Left joystick click | Toggle sprint |
| Right joystick left/right | Turn |
| Right joystick up/down | Change stance |
| Right joystick click | Jump |
| Left Menu button | Main menu |
| X | Open/close inventory |
| Y near ammo pouch | Change ammunition type |
| Y elsewhere | Native reload when supported |
| A | Release locked weapon action / change fire mode |
| B | Drop magazine |
| Weapon-hand trigger | Fire |
| Support-hand trigger near weapon mechanism | Operate mechanical parts |
| Grip near holster | Draw weapon |
| Hold grip | Keep weapon in hand |
| Support-hand grip | Two-handed hold |
| Grip near body slot | Take equipment |
| Free-hand trigger | Interact with objects |
| Trigger while pointing at UI | Click/select |

**Important:** Weapons remain held only while the grip is pressed. Physical reloading varies by weapon type.

For detailed controls, see [docs/VR.md](docs/VR.md).

## Known Issues

- Some weapons have incorrect reload behavior or animations.
- GAMMA modpack's weapon's alignment and mechanics remain inconsistent.
- Some physical reload interactions can become stuck.
- Certain shotguns may have unresolved jam-recovery problems.
- Scope and binocular support is incomplete.
- VR body and equipment slots may shift unexpectedly.
- Some modded script actions cannot be accessed through VR controllers.
- Left-handed and seated support require more testing.
- Full desktop/VR save interchangeability is unverified.
- Blood effects and desktop rendering require further regression testing.

## Development Status

This is an experimental alpha.

The November 1, 2026 audit recorded ten passing automated VR test suites, but did not establish a clean final binary or complete in-headset, desktop, and cross-mode save acceptance.

Features are being developed and tested incrementally. Not every supported system has been fully validated in-game.

## Bug Reports & Contributions

Issues, bug reports, compatibility feedback, and contributions are welcome.

When reporting a problem, please include:

- Headset and controller type.
- OpenXR runtime.
- Game and modpack configuration.
- Build or commit version.
- Steps to reproduce.
- Relevant logs or screenshots.

## Links & Acknowledgements

**VR fork:** [[Astork735/xray-monolith](https://github.com/Astork735/xray-monolith)](https://github.com/Astork735/xray-monolith)

**Original upstream project:** [[themrdemonized/xray-monolith](https://github.com/themrdemonized/xray-monolith)](https://github.com/themrdemonized/xray-monolith)

**Releases:** [[X-Ray Monolith VR Releases](https://github.com/Astork735/xray-monolith/releases)](https://github.com/Astork735/xray-monolith/releases)

This project builds on the work of the original X-Ray engine developers, the Anomaly modding community, themrdemonized, and the many upstream contributors.
