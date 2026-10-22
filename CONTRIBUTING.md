# Contributing

## Branches

- `community` (the default) is the build: the official port plus every fix we've taken in. Releases
  are tagged from it as `community-YYYYMMDD-HHMM`.
- `main` follows the official repository's `main` and nothing else.
- Every other branch is one change, branched from `main`, and is open as a pull request on the
  official repository. That way each fix can go upstream on its own, without the rest of this fork.

## Sending a change

Open a pull request against `community`. If the fix also applies to the official port, say so and
we'll send it upstream too, as a separate branch from `main` with you as the author.

Please test on at least one game and say which (Uncharted 1, 2 or 3), which game version (1.00 or
1.02, US or EU) and which GPU. Attach the relevant part of `user\last_run.log` for crash fixes.

## Changes from other forks and upstream pull requests

We look through the other forks and open upstream pull requests for fixes worth having in the
build. When we take one, we keep the original author on the commit (or credit them in the commit
message and in [CREDITS.md](CREDITS.md) when it had to be rewritten) and link back to where it came
from. Linux, macOS and Xbox-only changes aren't taken for now, because the build is Windows only.

## Building

MSYS2 CLANG64 on Windows: `bash build.sh` builds `out\bb-probe.exe`, and
`bash packaging/windows/package.sh` makes `dist\uncharted-windows` and the zip.
