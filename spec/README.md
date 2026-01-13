# TLA+ Verification Setup

This project includes formal verification of the `ref_owner` and `unique_reference` implementation using TLA+ specifications.

## Prerequisites

### Java Runtime

Both TLC and Apalache require Java to run. Install Java 11 or later:

**macOS (Homebrew):**
```bash
brew install openjdk@17
```

**Verify installation:**
```bash
java -version
```

### TLA+ Model Checkers

The CMake build will automatically download the model checkers if not found. You can also install them manually:

#### TLC (TLA+ Model Checker)

**Automatic (via CMake):**
CMake will automatically download TLC v1.8.0 when configuring if not found.

**macOS (Homebrew):**
```bash
brew install --cask tla+-toolbox
```

**Manual Installation:**
1. Download `tla2tools.jar` from https://github.com/tlaplus/tlaplus/releases
2. Place in `/usr/local/lib/` or `/opt/homebrew/lib/`

#### Apalache (Symbolic Model Checker)

**Automatic (via CMake):**
CMake will automatically download Apalache v0.52.1 when configuring if not found.

**macOS (Homebrew):**
```bash
brew install apalache
```

**Manual Installation:**
1. Download from https://github.com/apalache-mc/apalache/releases
2. Extract and add `bin/apalache-mc` to your PATH

## Running Verification

After installing the tools, reconfigure CMake:

```bash
cmake --preset clang-latest-debug
```

### Available Targets

**TLC Verification:**
```bash
cmake --build --preset clang-latest-debug --target verify-tlc-unique
cmake --build --preset clang-latest-debug --target verify-tlc-shareable
cmake --build --preset clang-latest-debug --target verify-tlc
```

**Apalache Verification:**
```bash
cmake --build --preset clang-latest-debug --target verify-apalache-unique
cmake --build --preset clang-latest-debug --target verify-apalache-shareable
cmake --build --preset clang-latest-debug --target verify-apalache
```

**Run All:**
```bash
cmake --build --preset clang-latest-debug --target verify-all
```

## Specifications

- `spec/UniqueReference.tla` - Formal specification of ref_owner and unique_reference
- `spec/ShareablePtr.tla` - Formal specification of shared ownership patterns
- `spec/TLA_VERIFICATION_GUIDE.md` - Detailed mapping between TLA+ and C++

## What Gets Verified

The model checkers verify critical safety properties:
- **NoUseAfterFree**: No client holds a reference to a deleted object
- **NoInvalidReference**: Cannot have refs and be deleted simultaneously
- **ReferencesAlwaysValid**: If any client has refs, object is not deleted
- **DeletionImpliesMarked**: Deletion only happens after marking
- **MovedRefStillValid**: Reference moves preserve validity
