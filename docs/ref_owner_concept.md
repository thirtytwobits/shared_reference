# ref_owner: Deterministic Lifetime with Safe Sharing

**A Concept Document for SG14 Discussion**

*Scott Dixon <sdixon@zoox.com>*

---

## The Problem in One Sentence

`std::shared_ptr` provides safe sharing but unpredictable destruction timing; `std::unique_ptr` provides predictable destruction but no sharing. **We need both.**

## The Idea

Introduce a smart pointer that:
- **Owns** an object with `unique_ptr` semantics (single owner, deterministic destruction)
- **Lends** non-owning references that are *statically guaranteed valid* (not nullable, no validity checks needed)
- **Coordinates** deletion timing explicitly via a simple protocol

```cpp
// Owner controls lifetime
ref_owner<Sensor> sensor(new Sensor());

// Consumers get guaranteed-valid references
unique_reference<Sensor> ref = sensor.make_ref();  // non-nullable, like reference_wrapper
ref.get().read();  // no validity check needed - it's a reference, not a pointer

// Owner decides exactly when deletion occurs
if (not sensor.mark_and_delete_if_ready()) // no new refs allowed, deletes iff ref_count == 0
{
    // this could be considered a loggable error in some systems
    // in others the object would be put into a deletion queue to be deleted later
    // the deletion will NOT occur asynchronously or magically. It requires the ref_owner
    // delete_if_deleteable method to be called. What's important is we can handle this
    // error explicitly as "did not delete *ON TIME* which is a failure in realtime systems
    // even if the object is, eventually, deleted. 
    // This is an example of how "garbage collection" can be built out of the ref_owner primitive.
}

```

## Why This Matters

**Real-time systems** require deterministic timing. Consider a hard real-time control loop:

```
                         FRAME (fixed period, e.g. 10ms)
    |<------------------------------------------------------------>|
    |                                                              |
    +------+----------+------------+----------+-----------+--------+
    |  1   |    2     |     3      |    4     |     5     |   6    |
    +------+----------+------------+----------+-----------+--------+
    |      |          |            |          |           |        |
    | MSG  |   MSG    |    MSG     | OPTIONAL |  MEMORY   | GUARD  |
    | LOAD | PROCESS  |   XMIT     |   WORK   | RECLAIM   |  BAND  |
    |      |          |            |          |           |        |
    +------+----------+------------+----------+-----------+--------+
       |                                           |
       |  unique_reference<Msg> used here          |  ref_owner<Msg>::delete_if_deleteable()
       |  (borrowed from memory pools)             |  called HERE - deterministic timing
       |                                           |
       +-------------------------------------------+
                Messages flow through phases 1-4,
                destruction ALWAYS happens in phase 5

    Phase 6 (Guard Band): Slack time absorbing timing variability.
    HARD constraint: execution SHALL NOT exceed frame boundary.
    May be nearly the entire frame or just 1 CPU cycle, but never zero.

    ^                                                              ^
    |                                                              |
    +--------------------------------------------------------------+
                            REPEATS EVERY FRAME
```

With `shared_ptr`, destruction happens whenever the last reference releases - could be phase 2, 3, or 4 depending on code paths. Unrelated changes (new feature, refactor) can shift which callsite is "last," causing **timing regressions** in high-criticality code.

With `ref_owner`, destruction is **always** in phase 5. The timing budget is explicit and auditable.

**Reference semantics** eliminate error paths. A `weak_ptr` user must check validity every access. A `unique_reference` holder knows it's valid - if they have it, it works. This removes defensive code paths that "can't happen" but must be written anyway.

**Explicit contracts** replace implicit assumptions. Today, APIs passing raw pointers rely on documentation: "don't store this" or "valid until X." `unique_reference` makes this contract type-safe and compiler-enforced.

## Current Scope: `unique_reference`

The initial proposal focuses on the single-owner case:

| Type | Role |
|------|------|
| `ref_owner<T>` | Owns the object, controls deletion timing |
| `unique_reference<T>` | Non-owning, non-nullable, move-only reference |
| `waitable_ref_owner<T>` | Adds blocking wait for all refs to release |

Key properties:
- Lock-free reference counting (atomics only)
- No heap allocation by the smart pointer itself
- Works with `-fno-exceptions`
- Formally verified with TLA+ (prototype includes spec)

## Future Direction: `shared_reference`

The owner semantics remain identical - single `ref_owner`, explicit deletion control. The difference is in borrower flexibility:

| Reference Type | Copyable | Use Case |
|----------------|----------|----------|
| `unique_reference<T>` | No (move-only) | Scoped access, clear handoff |
| `shared_reference<T>` | Yes | Multiple concurrent borrowers |

```cpp
ref_owner<Session> owner(new Session());

// shared_reference can be copied freely
shared_reference<Session> ref1 = owner.make_shared_ref();
shared_reference<Session> ref2 = ref1;  // both valid, both count

// Owner protocol unchanged
if (not owner.mark_and_delete_if_ready()) // no new refs allowed after this
{
    // same semantics as unique_ptr.
}
else
{
    // deletes iff ref1 and ref2 have been destroyed.
}
```

**Open challenge**: `shared_reference` requires a control block for its own reference counting (independent of the owner's count). Traditional implementations heap-allocate this. I'm exploring heap-free solutions before including this in the proposal - ideas welcome.

## What I'm Looking For

1. **Direction check**: Does this address a real gap in the standard library?
2. **Naming feedback**: `ref_owner` / `unique_reference` / `shared_reference`
3. **Scope guidance**: Start with `unique_reference` only, or include `shared_reference`?
4. **Design concerns**: Anything fundamentally problematic?
5. **`shared_reference` control block**: Ideas for heap-free reference counting among borrowers?

## Resources

- Full proposal draft: [`docs/ref_owner_proposal.md`](ref_owner_proposal.md)
- Working implementation: [`include/zoox/memory_w_unique_reference.h`](../include/zoox/memory_w_unique_reference.h)
- TLA+ specification: [`specs/UniqueReference.tla`](../specs/UniqueReference.tla)
- Unit tests with TSAN coverage

---

*This concept document accompanies a working prototype with formal verification. Happy to share the repository for deeper review.*
