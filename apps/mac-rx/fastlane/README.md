fastlane documentation
----

# Installation

Make sure you have the latest version of the Xcode command line tools installed:

```sh
xcode-select --install
```

For _fastlane_ installation instructions, see [Installing _fastlane_](https://docs.fastlane.tools/#installing-fastlane)

# Available Actions

## Mac

### mac build_lib

```sh
[bundle exec] fastlane mac build_lib
```

Build C++ library (soluna_core) for macOS

### mac beta

```sh
[bundle exec] fastlane mac beta
```

Build and upload to TestFlight

### mac release

```sh
[bundle exec] fastlane mac release
```

Submit to App Store

### mac build_local

```sh
[bundle exec] fastlane mac build_local
```

Build for local testing (no signing)

----

This README.md is auto-generated and will be re-generated every time [_fastlane_](https://fastlane.tools) is run.

More information about _fastlane_ can be found on [fastlane.tools](https://fastlane.tools).

The documentation of _fastlane_ can be found on [docs.fastlane.tools](https://docs.fastlane.tools).
