# Third-party code

## rexglue-sdk

A copy of the ReXGlue SDK v0.10.0, taken from
[Sirhalo23/rexglue-sdk](https://github.com/Sirhalo23/rexglue-sdk) at commit `c94f5eb`
(a snapshot of [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) v0.10.0).
It is kept in this repository so this project can change it, for example to add the
Xbox Live replacement needed for online play. See `rexglue-sdk/LICENSE`.

Changes from the original:
- `tools/binutils/` was left out (PowerPC tools used only by the SDK's own tests).
- The SDK's helper libraries (`rexglue-sdk/thirdparty/*`) are git submodules of this
  repository, pinned to the same commits the SDK uses.

- `src/system/CMakeLists.txt`: the runtime now exports imgui's header folder, so a
  game project can build against the SDK source tree (public SDK headers include
  `<imgui.h>`; the installed SDK package ships it, the source tree did not).

Any further changes made by this project are listed in the commit history.
