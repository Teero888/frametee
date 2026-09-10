# FrameTee SM64 lock tool

This release-build utility encrypts each generated native SM64 runtime with its
matching, normalized ROM. It uses the same `pwbox` sodium container format as
Wafel's `libsm64_lock`, so a correct ROM is cryptographically required to
recover the library.

FrameTee packages a precompiled copy for each supported desktop platform. End
users do not need Rust, a compiler, or a build tool; they only place a legally
obtained vanilla ROM in `data/games/sm64` next to the application.
