# Patches

Every change this port makes to third-party code, as diffs against the upstream sources it was taken from. The changed
files are also in the repository as they are, so it builds without applying anything: these patches show exactly
what changed, and let you apply the same changes to a fresh upstream checkout.

| Patch | Upstream base | What it changes |
|---|---|---|
| `rexglue-sdk-v0.10.0.patch` | [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) v0.10.0, commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` | The whole SDK fork (`sdk/`): the Horizon (Switch) layer, the code generator's options, the console tuning of the runtime, and the SDK's build of FFmpeg (`thirdparty/CMakeLists.txt`, `thirdparty/ffmpeg-overlay/codec_list.c`). 202 files, +25,814 / -889 lines |
| `ffmpeg-0604b46.patch` | [wmarti/FFmpeg](https://github.com/wmarti/FFmpeg) commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c`, the FFmpeg submodule of rexglue-sdk v0.10.0 | `config.h` (the Switch configuration), `libavcodec/fft_template.c` and `libavcodec/mdct_template.c` (a table cache), and two new files: `config_switch_aarch64.h` and `config_masseffect_wmv3.h` |
| `xenosrecomp-990d03b.patch` | [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) commit `990d03b28a27b50277ee5d8d942e1c5f873869d1` | The shader translator (`shaders/XenosRecomp`): `shader_recompiler.cpp`/`.h` and `shader_common.h`, and the new `pch_min.h`. What each change does is in `shaders/README.md` |

The NVK driver (Mesa) is not changed by this port: see [mesa/README.md](../mesa/README.md).

## Applying them

Each patch is a `git diff` from the root of its upstream repository:

```sh
git clone https://github.com/rexglue/rexglue-sdk.git && cd rexglue-sdk
git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git apply ../sa2-nx/patches/rexglue-sdk-v0.10.0.patch

git submodule update --init thirdparty/FFmpeg && cd thirdparty/FFmpeg
git apply ../../../sa2-nx/patches/ffmpeg-0604b46.patch

git clone https://github.com/hedge-dev/XenosRecomp.git && cd XenosRecomp
git checkout 990d03b28a27b50277ee5d8d942e1c5f873869d1
git apply ../sa2-nx/patches/xenosrecomp-990d03b.patch
```

The SDK patch leaves upstream's folders that the fork does not carry (`docs/`, `scripts/`, `tools/`, `.github/` and
the editor settings, and upstream's own files in `tests/`) as they are; nothing in the port's build uses them. In
`tests/` it adds the fork's eight unit tests (`sdk/tests/`, run by `tools\win\run_sdk_tests.ps1`). Line endings are
LF, as git stores the files.

Each patch was checked by applying it to a clean checkout of its base: the result is byte for byte the files of this
repository.

After a change to `sdk/`, `python tools/make_sdk_patch.py <scratch repository>` writes the SDK patch again (its
header says how to make the scratch repository) and prints the new totals for the table above.
