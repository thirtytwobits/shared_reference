# TLA+ Specification Verification Guide

This document provides instructions for verifying that the C++ implementation in `include/zoox/memory_w_unique_reference.h` correctly implements the TLA+ specification in `specs/UniqueReference.tla`.

## Overview

The `ref_owner` and `unique_reference` classes implement a lock-free reference counting mechanism with explicit deletion control. The TLA+ specification formally defines the safety properties and state transitions.

## Specification-to-Code Mapping

### Variables

| TLA+ Variable | C++ Member | Type | Description |
|---------------|------------|------|-------------|
| `refCount` | `ref_count_` | `std::atomic<size_t>` | Atomic reference count |
| `markedForDeletion` | `marked_for_deletion_` | `std::atomic<bool>` | Deletion flag |
| `deleted` | `deleted_` | `std::atomic<bool>` | Object destroyed flag |
| `clientRefs[c]` | (implicit) | unique_reference instances | Per-client ref tracking |

### Actions

| TLA+ Action | C++ Method | Key Behavior |
|-------------|------------|--------------|
| `TryMakeRefSuccess(c)` | `try_register_ref()` returning `true` | Optimistic increment, check, success |
| `TryMakeRefFail(c)` | `try_register_ref()` returning `false` | Optimistic increment, check, rollback |
| `ReleaseRef(c)` | `on_ref_released()` | Decrement refCount |
| `MarkForDeletion` | `mark_for_deletion()` | Set markedForDeletion flag |
| `DeleteIfDeleteable` | `delete_if_deleteable()` | CAS on deleted flag |
| `MoveRef(from, to)` | Converting move ctor, `static_reference_move` | Ownership transfer, refCount unchanged |
| `DynamicMoveRefFail(c)` | `dynamic_reference_move` returning nullopt | No-op, source unchanged |

### Safety Invariants

| TLA+ Invariant | Description | How to Verify |
|----------------|-------------|---------------|
| `NoUseAfterFree` | `deleted => (refCount = 0)` | Check delete_if_deleteable only succeeds when refCount=0 |
| `NoInvalidReference` | `~(deleted /\ refCount > 0)` | Verify CAS prevents race between delete and ref creation |
| `ReferencesAlwaysValid` | `(refCount > 0) => ~deleted` | Same as above, contrapositive |
| `DeletionImpliesMarked` | `deleted => markedForDeletion` | Check delete_if_deleteable requires marked |
| `MovedRefStillValid` | `(clientRefs[c] > 0) => ~deleted` | Moved refs remain valid |
| `MovePreservesRefCount` | refCount unchanged by moves | MoveRef has UNCHANGED refCount |
| `FailedMovePreservesSource` | Failed dynamic cast leaves source intact | DynamicMoveRefFail has UNCHANGED vars |

## Verification Checklist

When modifying `memory_w_unique_reference.h`, verify each change against the specification:

### 1. Atomic Operations Match Spec

- [ ] `try_register_ref()` uses seq_cst for both fetch_add and load
- [ ] The check for `markedForDeletion` happens AFTER the increment (optimistic)
- [ ] Rollback uses seq_cst for fetch_sub
- [ ] `on_ref_released()` uses seq_cst for fetch_sub
- [ ] `delete_if_deleteable()` uses CAS with acq_rel

### 2. Preconditions Match Spec

For each action, verify the preconditions:

```
TryMakeRefSuccess preconditions:
  ~markedForDeletion - checked after increment
  ~deleted           - implicit (if deleted, marked is true)

ReleaseRef preconditions:
  clientRefs[c] > 0  - ensured by unique_reference existing

MarkForDeletion preconditions:
  ~markedForDeletion - idempotent in C++ (safe to call multiple times)
  ~deleted           - safe to mark after delete (no-op)

DeleteIfDeleteable preconditions:
  markedForDeletion  - explicit check
  ~deleted           - explicit check + CAS
  refCount = 0       - explicit check
```

### 3. State Transitions Match Spec

Verify each method only modifies the variables specified:

```
TryMakeRefSuccess: 
  Changes: refCount (increment)
  Unchanged: markedForDeletion, deleted

TryMakeRefFail:
  Changes: none (rollback)
  Unchanged: all

ReleaseRef:
  Changes: refCount (decrement)
  Unchanged: markedForDeletion, deleted

MarkForDeletion:
  Changes: markedForDeletion (set true)
  Unchanged: refCount, deleted

DeleteIfDeleteable:
  Changes: deleted (set true via CAS)
  Unchanged: refCount, markedForDeletion
```

## Move Semantics Verification

The two-parameter template `unique_reference<ReferenceType, BaseType>` enables type-safe reference casting. The TLA+ spec models these as ownership transfers.

### Move Operations Mapping

| C++ Operation | TLA+ Action | Effect on refCount |
|---------------|-------------|-------------------|
| `unique_reference(unique_reference&&)` | `MoveRef(from, to)` | Unchanged |
| `static_reference_move<U>(ref&&)` | `MoveRef(from, to)` | Unchanged |
| `dynamic_reference_move<U>(ref&&)` success | `MoveRef(from, to)` | Unchanged |
| `dynamic_reference_move<U>(ref&&)` failure | `DynamicMoveRefFail(c)` | Unchanged |

### Key Insight: Moves Are Ownership Transfers

```cpp
// This is NOT creating a new reference - it's transferring ownership
unique_reference<Base, Derived> base_ref(std::move(derived_ref));
// derived_ref.owner_ is now nullptr
// base_ref.owner_ points to the same ref_owner
// ref_count_ is unchanged!
```

In TLA+ terms:
```
MoveRef(from, to) ==
    /\ clientRefs' = [clientRefs EXCEPT ![from] = @ - 1, ![to] = @ + 1]
    /\ UNCHANGED <<refCount, markedForDeletion, deleted>>
```

### Verifying Move Safety

1. **MovePreservesRefCount**: After any sequence of moves, `refCount` equals sum of all `clientRefs`
2. **MovedRefStillValid**: If a client holds a moved ref, the object is not deleted
3. **FailedMovePreservesSource**: A failed `dynamic_reference_move` leaves the source valid

## Common Verification Issues

### Issue 1: Memory Order Weakening

**Wrong:**
```cpp
ref_count_.fetch_add(1, std::memory_order_relaxed);  // WRONG
```

**Right:**
```cpp
ref_count_.fetch_add(1, std::memory_order_seq_cst);  // CORRECT
```

The spec assumes sequential consistency. Using weaker orderings may introduce races not caught by the model.

### Issue 2: Check-Before-Increment

**Wrong:**
```cpp
if (marked_for_deletion_.load()) return false;  // Check first
ref_count_.fetch_add(1);  // Then increment - RACE CONDITION
```

**Right:**
```cpp
ref_count_.fetch_add(1);  // Increment first (optimistic)
if (marked_for_deletion_.load()) {  // Then check
    ref_count_.fetch_sub(1);  // Rollback if needed
    return false;
}
```

The spec models the optimistic approach. Checking first creates a TOCTOU race.

### Issue 3: Missing CAS for Deletion

**Wrong:**
```cpp
deleted_.store(true);  // Direct store - RACE with other deleters
```

**Right:**
```cpp
bool expected = false;
if (deleted_.compare_exchange_strong(expected, true)) {
    // Only one thread succeeds
}
```

The spec's DeleteIfDeleteable implies exactly one deletion succeeds.

## Running Verification

### Model Checking (TLC)
```bash
bazel test //specs:unique_reference_model_check
```

### Type Checking (Apalache)
```bash
bazel test //specs:unique_reference_typecheck
```

### TSAN Testing
```bash
bazel test --config=tsan //:ref_owner_tla_test
```

### All Tests
```bash
bazel test //... --config=tsan
```

## Adding New Features

When adding new functionality to `ref_owner`:

1. **Define the TLA+ action first** in `specs/UniqueReference.tla`
2. **Add to Next relation** to include in model checking
3. **Run TLC** to verify safety invariants still hold
4. **Implement in C++** with spec comments showing correspondence
5. **Add tests** to `test/ref_owner_tla_test.cpp` derived from spec
6. **Run TSAN** to catch implementation races

## Spec Comment Format

All critical methods should include spec comments in this format:

```cpp
// =========================================================================
// TLA+ SPEC: ActionName
// =========================================================================
// ActionName ==
//     /\ precondition1
//     /\ precondition2
//     /\ variable' = new_value
//     /\ UNCHANGED <<other, variables>>
// =========================================================================
```

This format:
1. Clearly identifies the corresponding TLA+ action
2. Shows preconditions for manual verification
3. Shows state transitions for review

## Questions to Ask When Reviewing Changes

1. Does the C++ code check the same preconditions as the TLA+ spec?
2. Does the C++ code modify only the variables the spec says it should?
3. Are atomic operations using sequential consistency (or justified weaker)?
4. Is the optimistic increment + check + rollback pattern preserved?
5. Do the tests in `ref_owner_tla_test.cpp` cover this scenario?
6. Has the model been re-run with the new action included?

## AI Assistant Instructions

When asked to review or modify `memory_w_unique_reference.h`:

1. **Always read both files**: `include/zoox/memory_w_unique_reference.h` and `specs/UniqueReference.tla`
2. **Compare each C++ method** against its TLA+ action
3. **Verify safety invariants** are preserved by any changes
4. **Check atomic orderings** - default to seq_cst unless there's a documented reason
5. **Run TSAN tests** after making changes: `bazel test --config=tsan //:ref_owner_tla_test`
6. **Update spec comments** in C++ if the behavior changes
7. **Propose TLA+ spec changes** if new functionality is added
