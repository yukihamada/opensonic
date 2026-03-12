fastlane documentation
----

# Installation

Make sure you have the latest version of the Xcode command line tools installed:

```sh
xcode-select --install
```

For _fastlane_ installation instructions, see [Installing _fastlane_](https://docs.fastlane.tools/#installing-fastlane)

# Available Actions

## iOS

### ios build_lib

```sh
[bundle exec] fastlane ios build_lib
```

Build iOS library (soluna_core)

### ios create_app

```sh
[bundle exec] fastlane ios create_app
```

Create app on App Store Connect (requires Apple ID with Admin role)

### ios beta

```sh
[bundle exec] fastlane ios beta
```

Submit to TestFlight

### ios upload

```sh
[bundle exec] fastlane ios upload
```

Upload existing IPA to TestFlight (skip build)

### ios release

```sh
[bundle exec] fastlane ios release
```

Submit to App Store review with full metadata

### ios create_iap

```sh
[bundle exec] fastlane ios create_iap
```

Create In-App Purchase subscription on App Store Connect

### ios build_local

```sh
[bundle exec] fastlane ios build_local
```

Build for local testing (no signing)

----

This README.md is auto-generated and will be re-generated every time [_fastlane_](https://fastlane.tools) is run.

More information about _fastlane_ can be found on [fastlane.tools](https://fastlane.tools).

The documentation of _fastlane_ can be found on [docs.fastlane.tools](https://docs.fastlane.tools).
