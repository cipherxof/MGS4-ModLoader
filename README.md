# MGS4 Mod Loader

A simple mod loader for METAL GEAR SOLID 4 on PC. It can override files already stored in a PAK, replace resources embedded in slot files, add new virtual files, and merge stage CNF's without replacing `data.cnf`.

## Installation

Copy `MGS4ModLoader.asi` and `MGS4ModLoader.ini` into the game directory or an ASI-loader directory such as `scripts`. Mod packages always live beneath the MGS4 game directory's `mods` folder by default:

```text
MGS4/
  mods/
    your-mod-name/
```

Packages are sorted by folder name, case-insensitively, and a later package wins when multiple packages override the same virtual file.

## Slot files

Resources embedded in `ww/slotdat/*.slot` can be replaced without modifying or rebuilding the slot. Put loose replacements beneath a package's `slots` directory:

```text
mods/stage-model-test/
  slots/
    slot_s01a10l/
      0/
        s01a11a_east.mdn
        s01a11a_ground.mdn
```

## Stage CNF

Place `merge.cnf` or named `*.merge.cnf` files beside the stage's virtual `data.cnf` path inside a package:

```text
mods/tankbox/common/stage/stage00/r_sna01/TankBox.merge.cnf
```

## Building

Clone the repository and its dependencies:

```bash
git clone --recursive https://github.com/cipherxof/MGS4-ModLoader.git
cd MGS4-ModLoader
```

If the repository was cloned without `--recursive`, initialize the submodules first:

```bash
git submodule update --init --recursive
```

Windows with Visual Studio 2022:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Linux with MinGW-w64:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```


## Extra features (opt-in, all off by default)

Set them in `MGS4ModLoader.ini`:

```ini
; Only index these pack folders (case-insensitive); everything else in mods/ is ignored
ModPacks = BigBossTRUE

; Write the original data of every slot resource to logs/dump/<slot>/<page>/<id>.bin
DumpSlotResources = false

; Serve a slot page or a path prefix only while the player wears a given outfit / FaceCamo id
; ([linkvarbuf+0xB26] / [linkvarbuf+0xB27]). Several entries separated by ';'
SlotConditions = slot_sna_body/00005a25=outfit:6;slot_sna_face/00003e66=face:3
PathConditions = ww/textures/PC_TXN_UP/slot/slot_sna_body/00005a25/=outfit:6

; Display a HUD label as another text while the condition holds
HudNameEnabled = true
HudNameOutfit = 6
HudNameFace = 3
HudNameMatch = OLD SNAKE
HudNameReplace = BIG BOSS

; Add one row to the FaceCamo and/or Costume list of the Camouflage menu. The game keeps wearing
; the base id; the custom id only exists for the row, the conditions and the HUD name.
CustomFaceEnabled = true
CustomFaceId = 3
CustomFaceBaseId = 9
CustomFaceThumb = 07cf90
CustomFaceName = BIG BOSS (TRUE)
CustomFaceDesc = FaceCamo (Big Boss, true model)
CustomOutfitEnabled = true
CustomOutfitId = 6
CustomOutfitBaseId = 4
CustomOutfitThumb = 0607c4
CustomOutfitName = BIG BOSS (TRUE)
CustomOutfitDesc = Big Boss coat (true model)
```

Known ids (build `2026_0825`): costumes 1 = OctoCamo, 3 = Civilian Disguise, 4 = Suit, 7 = Altair;
FaceCamo 0 = none, 1 = FaceCamo, 2 = Young, 8 = Young w/ bandana, 4..7 = Beauties, 10 = Campbell,
11 = Otacon, 12 = Drebin, 13 = MGS1, 14/15 = Raiden A/B, 9 = Big Boss, 16/17 = gold variants.
