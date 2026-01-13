# PxxxxR0: `ref_owner` - A Smart Pointer with Deterministic Destruction

**Document Number:** PxxxxR0  
**Date:** 2026-01-10  
**Project:** Programming Language C++  
**Audience:** SG14, LEWG  
**Reply-to:** Scott Dixon <sdixon@zoox.com>

---

## I. Abstract

This paper proposes `ref_owner<T>`, a smart pointer that provides deterministic destruction timing by decoupling reference management from object lifetime. Unlike `shared_ptr`, where destruction occurs at an unspecified time when the last reference is released, `ref_owner` requires explicit owner-initiated deletion that only succeeds when no outstanding references exist. This design is essential for real-time systems where destructor execution must occur at predictable points in the execution cycle.

## II. Motivation and Scope

### The Problem with `shared_ptr` in Real-Time Systems

In real-time and low-latency systems, timing predictability is paramount. Systems often have well-defined execution cycles where specific operations must occur at specific times. Object destruction, which may involve non-trivial cleanup (deallocation, I/O, logging, etc.), must occur at controlled points in the cycle to maintain timing guarantees.

`std::shared_ptr` provides automatic lifetime management through reference counting, but this comes with a critical limitation: **destruction timing is undefined**. The destructor executes when the *last* reference is released, but which reference is "last" depends on:

- Order of scope exits across threads
- Order of container operations
- Compiler optimizations affecting destruction order
- Runtime conditions

This leads to a class of timing regressions that are:
1. **Difficult to diagnose** - Profiling shows destructor time appearing in "unrelated" code
2. **Non-deterministic** - The same code may exhibit different timing based on execution order
3. **Fragile** - Unrelated changes (adding a log statement, reordering operations) can shift which code path pays the destruction cost

**Plausible Real-World Example:**

In an autonomous vehicle system, a prediction component shares sensor data with multiple consumers via `shared_ptr`. During development, a seemingly innocuous change - adding telemetry logging that briefly holds a reference - caused the sensor data destructor to shift from a low-priority background thread to a high-criticality real-time path. The resulting latency spike violated timing requirements, despite no changes to the critical path code itself.

### The Solution: Explicit Deletion Control

`ref_owner` addresses this by separating two concerns:

1. **Reference tracking** - Lock-free, thread-safe reference counting
2. **Lifetime control** - Explicit, owner-initiated deletion

The owner (typically a component managing object lifecycle) controls *when* deletion occurs:

```cpp
class SensorDataManager {
    zoox::ref_owner<SensorData> current_data_;
    
public:
    // Called by consumers - safe, lock-free
    auto get_data_ref() { return current_data_.try_make_ref(); }
    
    // Called at deterministic point in execution cycle
    void end_of_cycle_cleanup() {
        current_data_.mark_for_deletion();
        // Destruction happens HERE, not wherever last consumer finishes
        current_data_.delete_if_deleteable();
    }
};
```

### Design Goals

1. **Deterministic destruction timing** - Owner controls when destruction occurs
2. **Lock-free reference operations** - No mutexes in the hot path
3. **Reference safety** - References cannot dangle; if you have one, the object is alive
4. **Familiar API** - Similar to `unique_ptr` for owners, `reference_wrapper` for consumers
5. **Formal verification** - Core invariants proven via TLA+ model checking

## III. Impact on the Standard

This proposal adds new class templates to `<memory>`:

- `ref_owner<T, Deleter>` - The owning pointer
- `unique_reference<T>` - Non-copyable reference handle
- `waitable_ref_owner<T, Deleter>` - Extension with blocking wait capability

No changes to existing standard library components are required.

### Relationship to Existing Facilities

| Facility | Ownership | Sharing | Destruction Timing |
|----------|-----------|---------|-------------------|
| `unique_ptr` | Exclusive | None | Deterministic (scope exit) |
| `shared_ptr` | Shared | Via copying | Non-deterministic (last release) |
| **`ref_owner`** | **Exclusive** | **Via references** | **Deterministic (explicit)** |

`ref_owner` fills a gap: shared access with deterministic destruction.

## IV. Rationale for Standardization

This section addresses why `ref_owner` belongs in the C++ standard library rather than remaining a third-party solution.

### Limited, Focused Scope

`ref_owner` is a **smart pointer**, not a memory management system. It manages exactly one object with a simple, well-defined lifecycle:

1. Owner creates the object
2. Owner distributes references to consumers
3. Owner marks for deletion when appropriate
4. Owner deletes when references are released

This is deliberately minimal. There are no:
- Garbage collection cycles or graph traversal
- Background threads or deferred reclamation queues
- Mark-and-sweep phases or generational boundaries
- Finalization ordering complexities

The scope is comparable to `unique_ptr` and `shared_ptr`: a single object, a clear ownership model, and predictable behavior. Users who need broader memory management can compose multiple `ref_owner` instances into larger systems, but the primitive itself remains simple.

### No Infrastructure Requirements

Unlike garbage collection schemes or deferred reclamation systems (hazard pointers, epoch-based reclamation, RCU), `ref_owner` requires no supporting infrastructure:

| Approach | Additional Infrastructure |
|----------|--------------------------|
| Traditional GC | Runtime, mark phases, collection threads |
| Hazard pointers | Thread-local lists, scan operations, retire queues |
| Epoch-based | Global epoch counter, per-thread epoch tracking |
| RCU | Grace period detection, callback queues |
| **`ref_owner`** | **None** |

`ref_owner` is entirely self-contained. Each instance manages its own reference count with standard atomics. There are no global data structures, no background processing, and no hidden coordination between instances. This makes the behavior predictable and the implementation auditable.

### Addressing a Gap in Standard Library Expressiveness

The C++ standard library provides excellent tools for exclusive ownership (`unique_ptr`) and shared ownership with automatic cleanup (`shared_ptr`). However, a common real-world pattern falls outside both:

> "Multiple components need concurrent read access to an object, but a single owner must control when destruction occurs."

Today, this pattern is typically implemented using raw pointers or references with implicit contracts:

```cpp
class DataProvider {
    std::unique_ptr<Data> data_;
public:
    Data* get_data() { return data_.get(); }  // "Trust us, it's valid"
};

class Consumer {
    Data* data_;  // Hope the provider outlives us
public:
    void process() { data_->read(); }  // No way to verify validity
};
```

This approach has well-known problems:

1. **Implicit contracts** - Correctness depends on documentation and discipline, not types
2. **Modularity barriers** - Components cannot be safely reused without understanding the entire system's lifetime guarantees
3. **Scaling difficulties** - As codebases grow, tracking which pointers are "known good" becomes increasingly error-prone
4. **Testing gaps** - The failure mode (use-after-free) is undefined behavior, making it difficult to write meaningful tests

The standard library should provide a vocabulary type for this pattern, just as it provides `shared_ptr` for shared ownership and `optional` for nullable values. `ref_owner` makes the contract explicit and verifiable:

```cpp
class DataProvider {
    std::ref_owner<Data> data_;
public:
    std::optional<std::unique_reference<Data>> get_data() {
        return data_.try_make_ref();  // Explicit: might not succeed
    }
};

class Consumer {
    std::unique_reference<Data> data_;  // Type guarantees validity
public:
    void process() { data_.get().read(); }  // If we have it, it's valid
};
```

### Enabling Safe Component Boundaries

A key benefit of standardization is establishing `ref_owner` and `unique_reference` as vocabulary types that can appear in component interfaces. Consider a plugin architecture:

```cpp
// Plugin API - raw pointer version
class IPlugin {
public:
    virtual void initialize(EngineState* state) = 0;  // Valid until...when?
};

// Plugin API - ref_owner version  
class IPlugin {
public:
    virtual void initialize(std::unique_reference<EngineState> state) = 0;
    // Clear: plugin holds a reference, must release before engine shutdown
};
```

With raw pointers, plugin authors must read documentation to understand lifetime requirements. With `unique_reference`, the type system communicates and enforces the contract. This becomes increasingly valuable as:

- Teams grow and institutional knowledge becomes harder to maintain
- Components are reused across projects with different lifetime characteristics
- Third-party code integrates with systems it wasn't originally designed for

### Complementing, Not Replacing, Existing Types

`ref_owner` is not intended to replace `shared_ptr`. The two serve different use cases:

- **Use `shared_ptr`** when any holder should be able to extend the object's lifetime
- **Use `ref_owner`** when one owner must control destruction timing

This is analogous to the relationship between `shared_ptr` and `unique_ptr`: both are smart pointers, both manage object lifetime, but they encode different ownership semantics. The standard library is richer for having both.

## V. Design Decisions

### Reference vs. Pointer Semantics for Consumers

A key design choice is that `unique_reference<T>` provides **reference semantics**, not pointer semantics:

```cpp
// shared_ptr style (pointer semantics)
std::shared_ptr<T> ptr = get_shared();
if (ptr) {  // Must check for null
    ptr->method();
}

// ref_owner style (reference semantics)
auto opt_ref = owner.try_make_ref();
if (opt_ref) {
    // No null check needed - if we have it, it's valid
    opt_ref->method();
    T& ref = *opt_ref;  // Direct reference access
}
```

This design:
- Eliminates null pointer bugs in consumer code
- Makes the API self-documenting (you have a *reference*, not ownership)
- Mirrors `std::reference_wrapper` semantics

### Lock-Free Reference Counting

Reference creation and destruction use an optimistic increment-then-check pattern:

```cpp
bool try_register_ref() noexcept {
    ref_count_.fetch_add(1, std::memory_order_seq_cst);
    if (marked_for_deletion_.load(std::memory_order_seq_cst)) {
        ref_count_.fetch_sub(1, std::memory_order_seq_cst);  // Rollback
        return false;
    }
    return true;
}
```

This provides:
- Wait-free reference creation (single atomic increment in common case)
- No mutex contention
- Predictable timing for reference operations

### Explicit Deletion Protocol

Deletion requires a two-phase protocol:

1. **`mark_for_deletion()`** - Prevents new reference creation
2. **`delete_if_deleteable()`** - Destroys object if no references exist

This separation allows:
- Non-blocking deletion attempts (return false if references exist)
- Integration with shutdown sequences
- Optional blocking via `waitable_ref_owner`

### Formal Verification

The reference counting logic has been formally verified using TLA+ model checking, proving:

- **NoUseAfterFree**: Deleted objects have zero references
- **NoInvalidReference**: References never point to deleted objects
- **DeletionImpliesMarked**: Objects can only be deleted after being marked

## VI. Comparison with Existing Solutions

This section examines how `ref_owner` relates to existing smart pointer implementations. Each library discussed here represents excellent engineering for its intended use cases. The analysis focuses on how design goals differ, particularly regarding deterministic destruction timing in real-time systems.

### Standard Library

#### `std::shared_ptr` + `std::weak_ptr`

`shared_ptr` and `weak_ptr` are the standard library's primary tools for shared ownership. They are well-designed, thoroughly tested, and appropriate for the vast majority of C++ applications.

```cpp
std::shared_ptr<T> owner = std::make_shared<T>();
std::weak_ptr<T> consumer_ref = owner;

// Consumer usage
if (auto locked = consumer_ref.lock()) {
    locked->method();
}
```

**Strengths:**
- Mature, battle-tested implementation available everywhere
- Automatic cleanup when last reference is released
- `make_shared` provides excellent cache locality with single allocation
- Well-understood semantics familiar to all C++ developers

**Different design goals for real-time systems:**

The `shared_ptr` design optimizes for convenience: objects are automatically deleted when the last reference is released. This is ideal for most applications but creates challenges in real-time systems where destruction timing must be predictable:

1. **Destruction timing** - When `locked` (above) goes out of scope, if `owner` was reset, destruction happens at that point rather than at a designated location in the execution cycle
2. **Nullable consumer access** - `lock()` can fail, requiring consumers to handle this case even when system design guarantees the object exists:

```cpp
// In a real-time control loop running at 100Hz:
void Consumer::on_cycle() {
    if (auto ptr = weak_ref_.lock()) {
        ptr->process();
    } else {
        // This branch should never execute in correct operation,
        // yet the code must handle it. What do we do here?
        // How do we cover this branch in simulation?
    }
}
```

With `unique_reference`, this conditional is eliminated at the type level:

```cpp
void Consumer::on_cycle() {
    // If we have the reference, it IS valid. No check needed.
    ref_.get().process();
}
```

#### `std::unique_ptr` + Lightweight References

For single-threaded code with clear ownership, `unique_ptr` combined with raw pointers or `reference_wrapper` is often the right choice:

```cpp
std::unique_ptr<T> owner = std::make_unique<T>();
T* consumer_ref = owner.get();
// or
std::reference_wrapper<T> ref = *owner;
```

**Strengths:**
- Zero overhead for the ownership relationship
- Clear, simple ownership semantics
- `reference_wrapper` provides non-nullable semantics

**Different design goals:**

These patterns work well when all code sharing the pointer can coordinate through design conventions. In concurrent systems or large codebases where the owner cannot easily coordinate with all consumers, runtime tracking becomes necessary.

### Boost Smart Pointers

Boost provides an excellent suite of smart pointers that have influenced the standard library and remain valuable for specialized use cases.

#### `boost::intrusive_ptr`

`intrusive_ptr` offers the most efficient reference counting by embedding the count in the managed object:

```cpp
class MyType : public boost::intrusive_ref_counter<MyType> {
    // Reference count embedded in object
};

boost::intrusive_ptr<MyType> ptr(new MyType());
```

**Strengths:**
- Single allocation (no separate control block)
- Cache-friendly: count and object are co-located
- Flexible: user controls the counting policy

**Different design goals:**

`intrusive_ptr` optimizes for memory efficiency and cache performance. Like `shared_ptr`, it uses last-release destruction semantics. For applications requiring owner-controlled destruction timing, an additional coordination mechanism would be needed.

#### `boost::local_shared_ptr`

Optimized `shared_ptr` variant for single-threaded use with reduced synchronization overhead.

**Strengths:**
- Faster than `shared_ptr` in single-threaded contexts
- Same familiar API

**Different design goals:**

Designed for performance in single-threaded code rather than cross-thread coordination with deterministic destruction.

### Concurrent Data Structure Libraries

#### Facebook Folly

Folly provides sophisticated concurrent data structures including `hazard_pointer` and `ReadMostlySharedPtr`.

`hazard_pointer` implements the hazard pointer pattern for lock-free memory reclamation:

```cpp
folly::hazard_pointer<T> hp;
T* ptr = hp.protect(shared_object);
// ptr is safe to use while hp is in scope
```

**Strengths:**
- Lock-free read access with excellent scalability
- Safe memory reclamation without garbage collection
- Well-suited for high-performance concurrent data structures

`ReadMostlySharedPtr` optimizes read-heavy workloads:

**Strengths:**
- Significantly faster reads than `shared_ptr` for read-mostly patterns
- Excellent for configuration objects read by many threads

**Different design goals:**

Folly's primitives are designed for building high-performance concurrent data structures. Hazard pointers use deferred reclamation (objects are deleted in batches after a grace period), which provides excellent throughput but not immediate, owner-controlled destruction. For the simpler pattern of "owner distributes references, owner decides when to delete," `ref_owner` provides a more direct solution.

#### EASTL (Electronic Arts STL)

EASTL is a high-quality STL alternative developed by Electronic Arts, optimized for game development:

**Strengths:**
- Excellent performance characteristics
- Memory allocation control
- Game-industry proven

EASTL's `shared_ptr` and `intrusive_ptr` follow standard semantics, optimized for game engine use cases.

### Embedded Systems Libraries

#### Embedded Template Library (ETL)

The [ETL](https://github.com/ETLCPP/etl) is an outstanding library for embedded systems development, actively maintained since 2014 with over 10,000 unit tests:

**Strengths:**
- No dynamic memory allocation - all storage is compile-time or stack allocated
- Fixed-capacity containers with STL-like API
- C++03 compatibility for older embedded toolchains
- Header-only for easy integration
- Deterministic behavior throughout

ETL provides useful reference utilities including `reference_flat_map`, observer pattern implementations, and type-erased delegates.

**Scope considerations:**

ETL deliberately focuses on deterministic, heap-free containers and utilities. Shared ownership patterns are explicitly outside its scope - a reasonable design choice that keeps the library focused. `ref_owner` addresses the specific case where shared access *is* needed but with the same determinism and heap-free guarantees that ETL provides for its containers.

#### Bloomberg BDE

Bloomberg's BDE library provides `bslma::ManagedPtr` with sophisticated allocator support:

**Strengths:**
- Flexible custom allocator integration
- Deleter type erasure
- Battle-tested in high-frequency trading systems

**Different design goals:**

BDE focuses on allocation flexibility rather than destruction timing control.

### Browser and Application Frameworks

#### Chromium Base Library

Chromium's `base::raw_ptr` and `base::raw_ref` provide memory-safety primitives:

```cpp
base::raw_ptr<Widget> ptr = GetWidget();
base::raw_ref<Widget> ref = *ptr;  // Non-nullable
```

**Strengths:**
- Non-nullable reference type (`raw_ref`)
- Dangling pointer detection in debug/asan builds
- Proven at massive scale in Chrome

**Different design goals:**

Chromium's primitives focus on catching bugs (dangling pointers) during development rather than providing runtime coordination between owners and consumers.

#### Qt Smart Pointers

Qt's `QSharedPointer` and `QWeakPointer` provide smart pointer functionality integrated with the Qt ecosystem:

**Strengths:**
- Excellent integration with Qt's object model
- Well-tested across platforms
- Familiar API for Qt developers

**Different design goals:**

Qt's pointers follow `shared_ptr` semantics, appropriate for GUI applications where deterministic destruction timing is less critical than in real-time control systems.

### Memory Reclamation Techniques

For completeness, we note two important concurrent programming patterns that address related problems.

#### Read-Copy-Update (RCU)

RCU, pioneered in the Linux kernel, provides lock-free reads with deferred destruction:

```cpp
// Simplified illustration
T* read_side() {
    rcu_read_lock();
    T* p = rcu_dereference(global_ptr);
    // use p safely
    rcu_read_unlock();
}

void write_side(T* new_val) {
    T* old = rcu_xchg_pointer(&global_ptr, new_val);
    synchronize_rcu();  // Wait for all readers
    delete old;         // Now safe
}
```

**Strengths:**
- Lock-free, wait-free reads with excellent scalability
- Owner controls the destruction point (after `synchronize_rcu`)
- Proven in production in the Linux kernel and high-performance systems

**Different design goals:**

RCU excels at the "replace object, wait for readers, delete old" pattern common in kernel data structures. The programming model requires explicit read-side critical sections and is designed for infrastructure code rather than application-level object sharing. `ref_owner` provides a simpler smart-pointer abstraction for the common case of "distribute references, then delete when ready."

#### Epoch-Based Reclamation

Epoch-based reclamation provides similar benefits to RCU with different tradeoffs:

**Strengths:**
- Excellent scalability for concurrent data structures
- Simpler implementation than full RCU in user-space

**Different design goals:**

Like RCU, epoch-based reclamation is designed for building concurrent data structure libraries. It uses batched destruction for throughput rather than immediate per-object destruction.

### Summary: Design Goal Comparison

The following table illustrates how different smart pointer designs prioritize different goals. Each design makes reasonable tradeoffs for its intended use cases.

| Design Goal | `shared_ptr` | `intrusive_ptr` | Hazard Ptr | ETL | `ref_owner` |
|-------------|-------------|-----------------|------------|-----|-----------------|
| Owner-controlled destruction | - | - | Partial | N/A | Yes |
| Lock-free reference ops | Partial | Yes | Yes | N/A | Yes |
| Non-nullable consumer refs | - | - | - | - | Yes |
| No heap for smart ptr | - | - | - | Yes | Yes |
| Shared access pattern | Yes | Yes | Yes | - | Yes |
| Automatic cleanup | Yes | Yes | Yes | - | - |
| Single allocation | Optional | Yes | - | N/A | - |

*N/A = ETL intentionally focuses on containers rather than shared ownership*

**Key observation:** `shared_ptr` and related designs optimize for *automatic* cleanup (delete when last reference released), which is ideal for most applications. `ref_owner` instead optimizes for *explicit* cleanup (owner decides when to delete), which is required when destruction timing must be predictable.

### Conclusion

Each library discussed above represents excellent engineering for its target use cases:

- **`shared_ptr`/`weak_ptr`** provide convenient automatic lifetime management suitable for most C++ applications
- **`intrusive_ptr`** offers cache-efficient reference counting for performance-critical code
- **Folly's hazard pointers** enable building high-performance lock-free data structures
- **ETL** provides deterministic, heap-free containers for embedded systems
- **RCU** powers scalable concurrent data structures in the Linux kernel and beyond

`ref_owner` addresses a specific gap: applications that need **shared access** (like `shared_ptr`) combined with **owner-controlled destruction timing** (unlike `shared_ptr`) and **non-nullable reference semantics** (unlike `weak_ptr`). This combination is particularly valuable in real-time systems where destruction must occur at predictable points in the execution cycle.

The design draws inspiration from these existing solutions:
- Reference semantics from `std::reference_wrapper`
- Lock-free counting techniques from `intrusive_ptr`
- Embedded-systems focus from ETL
- The owner-waits-for-readers pattern from RCU

## VII. Technical Specification

### Header `<memory>` Synopsis Additions

```cpp
namespace std {

// Forward declarations
template<class T, class Deleter = default_delete<T>>
class ref_owner;

template<class T>
class unique_reference;

template<class T, class Deleter = default_delete<T>>
class waitable_ref_owner;

// Exception type
class ref_owner_marked_exception : public runtime_error {
public:
    ref_owner_marked_exception();
};

} // namespace std
```

### Class Template `ref_owner`

```cpp
namespace std {

template<class T, class Deleter = default_delete<T>>
class ref_owner {
public:
    using element_type = T;
    using deleter_type = Deleter;
    using reference_type = unique_reference<T>;

    // Constructors
    explicit ref_owner(T* ptr);
    ref_owner(T* ptr, Deleter d);
    explicit ref_owner(unique_ptr<T, Deleter> ptr);

    // Destructor
    ~ref_owner();

    // Non-copyable
    ref_owner(const ref_owner&) = delete;
    ref_owner& operator=(const ref_owner&) = delete;

    // Movable
    ref_owner(ref_owner&& other) noexcept;
    ref_owner& operator=(ref_owner&& other) noexcept;

    // Observers
    T* get() const noexcept;
    T& operator*() const;
    T* operator->() const noexcept;
    explicit operator bool() const noexcept;

    // Reference creation
    optional<unique_reference<T>> try_make_ref() noexcept;
    unique_reference<T> make_ref();  // throws ref_owner_marked_exception

    // Reference queries
    bool has_outstanding_references() const noexcept;
    size_t ref_count() const noexcept;

    // Deletion control
    void mark_for_deletion() noexcept;
    bool is_marked_for_deletion() const noexcept;
    bool delete_if_deleteable() noexcept(is_nothrow_destructible_v<T>);
    bool is_deleted() const noexcept;

    // Convenience
    bool mark_and_delete_if_ready() noexcept(is_nothrow_destructible_v<T>);
};

} // namespace std
```

### Class Template `unique_reference`

```cpp
namespace std {

template<class T>
class unique_reference {
public:
    using type = T;

    // Non-copyable
    unique_reference(const unique_reference&) = delete;
    unique_reference& operator=(const unique_reference&) = delete;

    // Move-only
    unique_reference(unique_reference&& other) noexcept;
    unique_reference& operator=(unique_reference&&) = delete;

    // Converting move (for derived-to-base)
    template<class U>
        requires is_convertible_v<U*, T*>
    unique_reference(unique_reference<U>&& other) noexcept;

    // Destructor (releases reference)
    ~unique_reference() noexcept;

    // Access (reference semantics)
    T& get() const noexcept;
    operator T&() const noexcept;
    T& operator*() const noexcept;
    T* operator->() const noexcept;

    // Invocable support
    template<class... Args>
    auto operator()(Args&&... args) const
        -> invoke_result_t<T&, Args...>;
};

// Reference casting
template<class U, class T>
unique_reference<U> static_reference_move(unique_reference<T>&& ref) noexcept;

template<class U, class T>
optional<unique_reference<U>> dynamic_reference_move(unique_reference<T>&& ref) noexcept;

} // namespace std
```

### Class Template `waitable_ref_owner`

```cpp
namespace std {

template<class T, class Deleter = default_delete<T>>
class waitable_ref_owner : public ref_owner<T, Deleter> {
public:
    using ref_owner<T, Deleter>::ref_owner;

    // Blocking deletion
    void mark_and_wait_for_deletion();

    template<class Rep, class Period>
    bool mark_and_wait_for_deletion(chrono::duration<Rep, Period> timeout);

    template<class Clock, class Duration>
    bool mark_and_wait_until_deletion(chrono::time_point<Clock, Duration> deadline);
};

} // namespace std
```

## VIII. Example Usage

### Basic Usage

```cpp
#include <memory>

// Owner creates the ref_owner
std::ref_owner<Widget> owner(new Widget(args...));

// Consumers obtain references
auto ref = owner.try_make_ref();
if (ref) {
    ref->do_work();
}  // Reference released here

// Owner controls deletion timing
owner.mark_for_deletion();
while (!owner.delete_if_deleteable()) {
    // Wait for consumers to finish
    std::this_thread::yield();
}
```

### Real-Time System Pattern

```cpp
class RealTimeComponent {
    std::ref_owner<SensorData> data_;
    
public:
    // Called by real-time consumers (lock-free)
    auto get_sensor_ref() { 
        return data_.try_make_ref(); 
    }
    
    // Called at end of each cycle (deterministic timing)
    void cycle_end() {
        if (data_.is_marked_for_deletion()) {
            data_.delete_if_deleteable();  // Destruction happens HERE
        }
    }
    
    // Called when replacing sensor data
    void update_data(std::unique_ptr<SensorData> new_data) {
        data_.mark_for_deletion();
        data_ = std::ref_owner<SensorData>(std::move(new_data));
    }
};
```

### With Blocking Wait

```cpp
// For non-real-time shutdown sequences
std::waitable_ref_owner<Resource> resource(new Resource());

// ... resource used by multiple threads ...

// Clean shutdown - blocks until all references released
resource.mark_and_wait_for_deletion();
// Resource is guaranteed destroyed here
```

## IX. Implementation Experience

A production implementation has been deployed in autonomous vehicle systems where:

- Reference operations occur in microsecond-latency critical paths
- Destruction timing must be deterministic for safety certification
- Multiple components share data across thread boundaries

The implementation has been:
- Formally verified using TLA+ model checking
- Stress-tested with Thread Sanitizer (TSAN)
- Benchmarked against `shared_ptr` (comparable reference counting performance)

## X. Heap-Free Operation

### Motivation for Heap-Free Smart Pointers

In safety-critical and hard real-time systems, dynamic memory allocation is often prohibited or severely restricted because:

1. **Deterministic timing** - `malloc`/`new` have unbounded worst-case execution time
2. **Fragmentation avoidance** - Long-running systems cannot risk heap fragmentation
3. **Certification requirements** - Standards like DO-178C and ISO 26262 may require static memory allocation
4. **Memory-constrained environments** - Embedded systems with limited RAM and no MMU

`ref_owner` supports fully heap-free operation when combined with:
- Placement new for object construction
- A custom deleter that only invokes the destructor (no deallocation)

### Implementation Details

All `ref_owner` internal state is stored inline:

```cpp
template<class T, class Deleter>
class ref_owner {
    std::unique_ptr<T, Deleter> owned_ptr_;  // Pointer + deleter (inline)
    std::atomic<size_t> ref_count_{0};       // Inline
    std::atomic<bool> marked_for_deletion_{false};  // Inline
    std::atomic<bool> deleted_{false};       // Inline
};
```

`unique_reference` contains only a back-pointer to the owner:

```cpp
template<class T>
class unique_reference {
    ref_owner<T>* owner_;  // Single pointer
};
```

No internal operations allocate memory. The only potential allocation is the managed object itself, which users control via the Deleter.

### Example: Stack-Allocated Managed Object

```cpp
#include <memory>
#include <new>

// Deleter that only destructs, does not deallocate
template<class T>
struct destruct_only {
    void operator()(T* p) const noexcept {
        p->~T();
    }
};

void heap_free_example() {
    // Stack buffer for the managed object
    alignas(Widget) std::byte buffer[sizeof(Widget)];
    
    // Construct object in stack buffer
    Widget* widget = new (buffer) Widget(42);
    
    // Create ref_owner with destruct-only deleter
    // No heap allocation occurs here or during reference operations
    std::ref_owner<Widget, destruct_only<Widget>> ptr(widget);
    
    // References can be created and destroyed - all stack operations
    {
        auto ref = ptr.try_make_ref();
        if (ref) {
            ref->do_work();
        }
    }  // ref destroyed, ref_count decremented (no heap)
    
    // Explicit deletion runs destructor via destruct_only
    ptr.mark_and_delete_if_ready();
    // Widget::~Widget() called, but buffer remains on stack
}
```

### Example: Static Pool Allocation

For systems requiring a fixed pool of shareable objects:

```cpp
template<class T, size_t PoolSize>
class static_shareable_pool {
    struct slot {
        alignas(T) std::byte storage[sizeof(T)];
        std::ref_owner<T, destruct_only<T>> ptr{nullptr};
        bool in_use{false};
    };
    
    std::array<slot, PoolSize> pool_;
    
public:
    // Allocate from pool, returns ref_owner managing pool slot
    template<class... Args>
    std::ref_owner<T, destruct_only<T>>* allocate(Args&&... args) {
        for (auto& slot : pool_) {
            if (!slot.in_use) {
                T* obj = new (slot.storage) T(std::forward<Args>(args)...);
                slot.ptr = std::ref_owner<T, destruct_only<T>>(obj);
                slot.in_use = true;
                return &slot.ptr;
            }
        }
        return nullptr;  // Pool exhausted
    }
    
    // Return slot to pool after deletion
    void deallocate(std::ref_owner<T, destruct_only<T>>* ptr) {
        for (auto& slot : pool_) {
            if (&slot.ptr == ptr) {
                slot.in_use = false;
                return;
            }
        }
    }
};

// Usage in real-time system
static_shareable_pool<SensorData, 16> sensor_pool;  // Static allocation

void real_time_cycle() {
    auto* ptr = sensor_pool.allocate(current_readings);
    
    // Distribute references to consumers
    auto ref = ptr->try_make_ref();
    consumer.process(std::move(ref));
    
    // At deterministic point in cycle
    ptr->mark_for_deletion();
    if (ptr->delete_if_deleteable()) {
        sensor_pool.deallocate(ptr);
    }
}
```

### Caveat: `waitable_ref_owner`

The `waitable_ref_owner` variant adds `std::mutex` and `std::condition_variable` for blocking waits. While most implementations store these inline (POSIX pthreads), this is implementation-defined. Systems requiring guaranteed heap-free operation should use base `ref_owner` with application-level synchronization:

```cpp
// Heap-free alternative to waitable_ref_owner
std::ref_owner<T, destruct_only<T>> ptr(obj);

// Application-managed synchronization
while (ptr.has_outstanding_references()) {
    // Spin, yield, or use platform-specific wait primitive
    std::this_thread::yield();
}
ptr.delete_if_deleteable();
```

## XI. Exception-Free Operation (`-fno-exceptions`)

### Motivation

Many embedded systems, particularly microcontrollers (MCUs) and safety-critical applications, are compiled with exceptions disabled (`-fno-exceptions` in GCC/Clang). This is common because:

1. **Code size** - Exception handling infrastructure adds significant binary size, often unacceptable on memory-constrained MCUs (e.g., 32KB-256KB flash)
2. **Deterministic timing** - Exception unwinding has unpredictable execution time
3. **Stack usage** - Exception handling requires additional stack space that may not be available
4. **Certification** - Some safety standards discourage or prohibit exceptions
5. **Toolchain limitations** - Some embedded toolchains have incomplete or buggy exception support

A smart pointer intended for embedded use must work correctly when exceptions are disabled.

### Design for Exception-Free Builds

`ref_owner` is designed to be fully functional without exceptions:

**Non-throwing API as primary interface:**

```cpp
// Primary API - always available, never throws
std::optional<std::unique_reference<T>> try_make_ref();

// All core operations are noexcept
bool has_outstanding_references() const noexcept;
void mark_for_deletion() noexcept;
bool delete_if_deleteable() noexcept(...);  // Conditional on T's destructor
bool mark_and_delete_if_ready() noexcept(...);
T* get() noexcept;
```

The `try_make_ref()` method returns an optional, communicating failure through the return value rather than exceptions. This is the recommended API for all code, but especially for exception-free builds.

**Throwing API conditionally available:**

```cpp
#ifdef __cpp_exceptions
// Convenience API - only available when exceptions are enabled
std::unique_reference<T> make_ref();  // Throws ref_owner_marked_exception on failure

class ref_owner_marked_exception : public std::runtime_error { ... };
#endif
```

The `make_ref()` method and the exception type are conditionally compiled using the standard `__cpp_exceptions` feature test macro. Code compiled with `-fno-exceptions` simply does not have access to these APIs - there is no undefined behavior or linker error, they are cleanly absent.

**Debug assertions adapt to environment:**

```cpp
~ref_owner() {
#ifndef NDEBUG
    if (has_outstanding_references()) {
#ifdef __cpp_exceptions
        throw std::logic_error("destroyed with outstanding references");
#else
        std::abort();  // Or platform-specific error handling
#endif
    }
#endif
    delete_if_deleteable();
}
```

Debug-mode checks use exceptions when available, but fall back to `std::abort()` or similar when exceptions are disabled. Release builds skip these checks entirely.

### Example: MCU Usage

```cpp
// Compiled with: arm-none-eabi-g++ -fno-exceptions -fno-rtti

#include <ref_owner.h>

// Custom optional for pre-C++17 or memory-constrained environments
template<typename T> class mcu_optional { /* ... */ };

// Stack-allocated sensor with destruct-only deleter
struct destruct_only {
    template<typename T>
    void operator()(T* p) const noexcept { p->~T(); }
};

alignas(SensorData) std::byte sensor_buffer[sizeof(SensorData)];

std::ref_owner<SensorData, mcu_optional, destruct_only> sensor{
    new (sensor_buffer) SensorData()
};

void isr_handler() {
    // Non-throwing reference acquisition
    auto ref = sensor.try_make_ref();
    if (ref) {
        ref->process_interrupt();
    }
    // Reference automatically released
}

void main_loop() {
    while (true) {
        // ... cycle work ...
        
        if (should_reconfigure_sensor()) {
            sensor.mark_for_deletion();
            if (sensor.delete_if_deleteable()) {
                // Reconstruct in same buffer
                new (sensor_buffer) SensorData(new_config);
                sensor = std::ref_owner<SensorData, mcu_optional, destruct_only>{
                    reinterpret_cast<SensorData*>(sensor_buffer)
                };
            }
        }
    }
}
```

### Compatibility Matrix

| Feature | Exceptions Enabled | `-fno-exceptions` |
|---------|-------------------|-------------------|
| `try_make_ref()` | Available | Available |
| `make_ref()` | Available | Not available |
| `ref_owner_marked_exception` | Defined | Not defined |
| Debug assertions | Throw on violation | Abort on violation |
| All other operations | Full functionality | Full functionality |

### Recommendation for Standardization

The standard specification should:

1. Define `try_make_ref()` as the primary, always-available API
2. Specify `make_ref()` and `ref_owner_marked_exception` as conditionally available based on `__cpp_exceptions`
3. Leave debug-mode behavior implementation-defined (implementations may abort, log, or take other action when exceptions are unavailable)

This approach follows the precedent set by other standard library components that adapt to exception availability (e.g., `std::vector::at()` vs `operator[]`).

## XII. Dependency on `std::optional`

### Proposed Standard

The proposed standard interface uses `std::optional<unique_reference<T>>` as the return type for `try_make_ref()`. This is the natural choice because:

1. It clearly communicates fallibility (reference creation may fail if marked)
2. It composes well with existing code using `optional`
3. It provides the standard vocabulary type users expect

### Prototype Implementation Note

The reference implementation provided with this proposal includes a template parameter `OptionalT` that allows substituting alternative optional-like types:

```cpp
template<class T, 
         template<class> class OptionalT = std::optional,
         class Deleter = std::default_delete<T>>
class ref_owner;
```

This exists solely to enable use in C++14 codebases (where `std::optional` may not be available) and for compatibility with alternative optional implementations (e.g., `boost::optional`). 

**For standardization, we propose removing this template parameter** and directly depending on `std::optional`. The C++17 standard already includes `std::optional`, and any new standard library additions can reasonably depend on it.

## XIII. Open Questions

1. **Naming**: Is `ref_owner` the best name? Alternatives considered:
   - `owning_ptr` (emphasizes exclusive ownership)
   - `explicit_ptr` (emphasizes explicit deletion)
   - `controlled_ptr` (emphasizes controlled lifetime)

2. **`unique_reference` naming**: Should this be `borrowed_ref` or `ref_handle`?

3. **Exception vs. Expected**: Should `make_ref()` throw, or return `std::expected`?

4. **Allocator support**: Should `ref_owner` support allocators like `shared_ptr`?

## XIV. Future Work

The following features are not implemented in the current prototype and require further consideration:

### Reference Cast Operations

The prototype implements `static_reference_move` and `dynamic_reference_move` for type conversions, but does not provide equivalents for:

**`const_reference_move`** - Adding or removing `const` qualification:

```cpp
unique_reference<T> ref = ...;
unique_reference<const T> const_ref = const_reference_move<const T>(std::move(ref));
```

Considerations:
- Adding `const` is always safe and could be implicit via converting constructor
- Removing `const` (const_cast semantics) is dangerous and may not be appropriate for a safe reference type
- Should `unique_reference<const T>` be implicitly constructible from `unique_reference<T>`?

**`reinterpret_reference_move`** - Reinterpreting the referenced type:

```cpp
unique_reference<T> ref = ...;
unique_reference<U> reinterpreted = reinterpret_reference_move<U>(std::move(ref));
```

Considerations:
- `reinterpret_cast` semantics are inherently unsafe
- May be necessary for low-level code (hardware registers, memory-mapped I/O)
- Could undermine the safety guarantees `unique_reference` is designed to provide
- Alternative: require users to access via `get()` and use raw `reinterpret_cast`

### Recommended Approach

For const-correctness, we lean toward:
- Implicit conversion from `unique_reference<T>` to `unique_reference<const T>` (safe)
- No `const_reference_move` for removing const (unsafe, use `get()` if truly needed)

For reinterpretation:
- Do not provide `reinterpret_reference_move` (too unsafe for a safety-focused type)
- Users requiring reinterpret semantics should use `ref.get()` and cast the raw reference

These decisions should be validated by SG14 feedback on real-world use cases.

## XV. Acknowledgments

Thanks to the SG14 community for discussions on real-time memory management patterns.

## XVI. References

- [P0040R3] Friedman, B. "Extending memory management tools" 
- [P0843R14] Fernandes, G. "inplace_vector"
- [P1709R5] Revzin, B. et al. "Graph Library"
- [N4860] ISO/IEC 14882:2020 Programming Languages - C++

## XVII. Revision History

**R0** (2026-01-10)
- Initial revision

---

## Appendix A: Reference Implementation

A complete reference implementation is available at: [repository URL]

The implementation includes:
- Full `ref_owner`, `unique_reference`, and `waitable_ref_owner`
- TLA+ formal specification (`specs/UniqueReference.tla`)
- Comprehensive test suite with TSAN coverage
- Benchmarks comparing to `std::shared_ptr`

## Appendix B: TLA+ Specification

The core safety invariants are formally specified and verified:

```tla
VARIABLES refCount, markedForDeletion, deleted, clientRefs

SafetyInvariant ==
    /\ (deleted => refCount = 0)           \* NoUseAfterFree
    /\ ~(deleted /\ refCount > 0)          \* NoInvalidReference
    /\ (refCount > 0 => ~deleted)          \* ReferencesAlwaysValid
    /\ (deleted => markedForDeletion)      \* DeletionImpliesMarked
```

The TLC model checker exhaustively verifies these invariants hold across all possible interleavings of concurrent operations.
