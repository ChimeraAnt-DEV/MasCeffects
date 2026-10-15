# MasCeffects — repository notes for agents

## What this repo is

A Bedrock Edition native mod (`.chimerapack`) for GlowberryClient, porting the
Java Edition mod Mas Effects. It links against the preloader SDK published at
[ChimeraAnt-DEV/preloader-android](https://github.com/ChimeraAnt-DEV/preloader-android).

## Build

```sh
export PRELOADER_ROOT=/path/to/preloader-android
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build-chimerapack.sh --clean
```

Output: `dist/arm64-v8a/masceffects.chimerapack`.

The script runs the host unit tests **before** packaging, so a broken effect
rule fails the build rather than shipping.

## Pitfall: not every `pl::` symbol in a public header is actually exported

The preloader is compiled with `-fvisibility=hidden`, so only symbols carrying
`PL_EXPORT` (which expands to `__attribute__((visibility("default")))`) end up in
its dynamic symbol table. A mod links against the preloader's exports at load
time, so **importing a symbol the preloader does not export fails the load with
"missing symbol"** — the mod does not run at all.

Two SDK members look public but are **not** exported, and importing either one
breaks the mod:

| Symbol | Header | Reality |
| --- | --- | --- |
| `pl::cosmetics::GlesFunctions::usable()` | `pl/cosmetics/NativeCosmeticRenderer.hpp` | Declared on a plain `struct` with no `PL_EXPORT`. Built as `LOCAL HIDDEN`. |
| `pl::memory::resolveVtableSlotReferencingString(...)` | `pl/memory/Signature.hpp` | Declared `PL_EXPORT`, but the built library still carries it as `LOCAL HIDDEN`. |

`pl::cosmetics::GlesFunctions` is a **struct**, not a class, so a method on it
does not inherit visibility the way a `class PL_EXPORT Foo` does. Do not assume
a `PL_EXPORT` on a free function guarantees the export either — verify.

### How to check before shipping

Resolve the SDK's real exports and diff them against the mod's imports. Build the
preloader once, then:

```sh
READELF=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf

# Functions the preloader actually exports.
$READELF --dyn-syms libpreloader.so | awk '$7!="UND" && $4=="FUNC" {print $8}' | sort -u > have.txt

# pl:: symbols the mod imports.
$READELF --dyn-syms libmasceffects.so | awk '$7=="UND" {print $8}' | grep '^_ZN2pl' | sort -u > want.txt

# Must print nothing.
comm -23 want.txt have.txt
```

`./scripts/check-symbols.sh` runs exactly this when `PRELOADER_SO` points at a
built `libpreloader.so`.

### Safe alternatives

- Instead of `GlesFunctions::usable()`, test the individual function pointers
  (`if (!gles_.GetIntegerv || !gles_.DrawArrays) ...`). Reading the fields needs
  no symbol from the preloader.
- Instead of `resolveVtableSlotReferencingString`, use
  `pl::memory::resolveVtableFunction(typeInfoName, slot, module)` with the slot
  index the client's rules verify, and keep the executable-address guard so a
  stale index fails closed.

## Reading game memory

`GameFeed` is the only file that touches game memory. It hooks the **verified**
`LivePlayerRenderer` entry (slot 17) and captures actor pointers as the game
renders them. `pl::memory::hook` **chains** onto an existing detour rather than
replacing it, so it composes with the preloader's own hook on the same slot.

Every read is gated: a captured pointer must read as a real polymorphic object
with a well-formed RTTI name and a finite world position, and a health reading
must be finite. The feed installs separately from the render callback, so a build
where it cannot resolve loses only the hitboxes and the death effect.

The `Actor` offsets (`0x230` position, `0x238` rotation, `0x260` health) come
from the preloader's own local-player feed, except health, which is **not**
verified there — the preloader's `nativeReadLocalPlayerHealth` is a stub. Health
is therefore plausibility-checked and the rejected count is surfaced in the
diagnostics line rather than trusted silently.
