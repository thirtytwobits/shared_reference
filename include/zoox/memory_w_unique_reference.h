/**
 * @file
 * @brief Shared
 * @copyright Highly Confidential and Proprietary Zoox Information. Copyright
 * (c) Zoox.
 */
#ifndef ZOOX_MEMORY_W_UNIQUE_REFERENCE_H
#define ZOOX_MEMORY_W_UNIQUE_REFERENCE_H

// =============================================================================
// zoox::ref_owner - A Smart Pointer with Explicit Deletion Control
// =============================================================================
//
// OVERVIEW
// --------
// ref_owner is a smart pointer that wraps std::unique_ptr and provides
// explicit control over when the managed object is deleted. Unlike shared_ptr,
// deletion does not happen automatically when the last reference is released.
// Instead, the owner must explicitly request deletion, which only succeeds
// when no outstanding references exist.
//
// This design is ideal for scenarios where:
//   - Object lifetime is managed by an owner, but references are shared
//   - Deletion timing must be controlled (e.g., during shutdown sequences)
//   - Lock-free reference counting is required for performance
//   - You need to wait for all users to finish before destroying an object
//
// KEY CONCEPTS
// ------------
// 1. ref_owner<T>    - The owning pointer (like unique_ptr with ref tracking)
// 2. unique_reference<T> - A non-copyable reference (like reference_wrapper)
// 3. Explicit deletion   - Owner calls mark_for_deletion() + delete_if_deleteable()
//
// BASIC USAGE
// -----------
//
//   // 1. Create a ref_owner (owner)
//   zoox::ref_owner<MyClass> owner(new MyClass(args...));
//
//   // 2. Create references for other users
//   auto ref = owner.make_ref();           // Throws if marked for deletion
//   auto opt_ref = owner.try_make_ref();   // Returns empty optional if marked
//
//   // 3. Use the reference like a reference_wrapper
//   ref.get().doSomething();    // Access via get()
//   ref->doSomething();         // Or arrow operator
//   MyClass& obj = ref;         // Implicit conversion to T&
//
//   // 4. When done, mark for deletion and delete
//   owner.mark_for_deletion();             // Prevents new references
//   bool deleted = owner.delete_if_deleteable();  // Deletes if no refs
//
//   // Or use the convenience method:
//   owner.mark_and_delete_if_ready();
//
// DELETION PROTOCOL
// -----------------
// The owner must follow this protocol:
//
//   1. Call mark_for_deletion() - After this, no new refs can be created
//   2. Wait for has_outstanding_references() == false
//   3. Call delete_if_deleteable() - Actually destroys the object
//
// For blocking wait, use waitable_ref_owner:
//
//   zoox::waitable_ref_owner<MyClass> owner(new MyClass());
//   // ... refs created and used ...
//   owner.mark_and_wait_for_deletion();  // Blocks until all refs released
//
// REFERENCE TYPES
// ---------------
// unique_reference<T> is similar to std::reference_wrapper but:
//   - Non-copyable (unique ownership of the reference slot)
//   - Move-only (transfers ownership)
//   - Automatically decrements ref count on destruction
//   - Provides get(), operator*, operator->, and implicit conversion
//
// Example with polymorphism:
//
//   zoox::ref_owner<Derived> ptr(new Derived());
//   auto derived_ref = ptr.make_ref();
//
//   // Convert to base reference (compile-time checked)
//   zoox::unique_reference<Base, Derived> base_ref(std::move(derived_ref));
//
//   // Or use the helper function:
//   auto base_ref = zoox::static_reference_move<Base>(std::move(derived_ref));
//
//   // Runtime-checked downcast:
//   auto maybe_derived = zoox::dynamic_reference_move<Derived>(std::move(base_ref));
//   if (maybe_derived) {
//       maybe_derived->derivedMethod();
//   }
//
// CUSTOM DELETERS
// ---------------
// Like std::unique_ptr, ref_owner supports custom deleters:
//
//   // Functor deleter
//   struct MyDeleter {
//       void operator()(MyClass* p) { /* custom cleanup */ delete p; }
//   };
//   zoox::ref_owner<MyClass, std::optional, MyDeleter> ptr(new MyClass());
//
//   // Lambda deleter
//   auto deleter = [](MyClass* p) { cleanup(p); delete p; };
//   zoox::ref_owner<MyClass, std::optional, decltype(deleter)>
//       ptr(new MyClass(), deleter);
//
//   // From unique_ptr with custom deleter
//   auto uptr = std::unique_ptr<MyClass, MyDeleter>(new MyClass());
//   zoox::ref_owner<MyClass, std::optional, MyDeleter> ptr(std::move(uptr));
//
// CUSTOM OPTIONAL TYPE
// --------------------
// The OptionalT template parameter allows using alternative optional types:
//
//   // Default: std::optional
//   zoox::ref_owner<MyClass> ptr(new MyClass());
//
//   // With boost::optional
//   zoox::ref_owner<MyClass, boost::optional> ptr(new MyClass());
//
// THREAD SAFETY
// -------------
// - Reference creation/destruction is lock-free and thread-safe
// - Multiple threads can safely create and destroy references concurrently
// - mark_for_deletion() is thread-safe
// - delete_if_deleteable() uses CAS to ensure only one thread deletes
//
// For waiting on reference release, use waitable_ref_owner which adds
// mutex/condition_variable for efficient blocking.
//
// TEMPLATE PARAMETERS
// -------------------
//   template <typename T,
//             template <typename> class OptionalT = std::optional,
//             typename Deleter = std::default_delete<T>>
//   class ref_owner;
//
//   template <typename ReferenceType,
//             typename BaseType = ReferenceType,
//             template <typename> class OptionalT = std::optional,
//             typename Deleter = std::default_delete<BaseType>>
//   class unique_reference;
//
// FORMAL VERIFICATION
// -------------------
// The reference counting logic has been formally verified using TLA+.
// See specs/UniqueReference.tla for the specification and proofs of:
//   - NoUseAfterFree: deleted => (refCount = 0)
//   - NoInvalidReference: ~(deleted /\ refCount > 0)
//   - ReferencesAlwaysValid: (refCount > 0) => ~deleted
//   - DeletionImpliesMarked: deleted => markedForDeletion
//
// =============================================================================

#include <memory>
#include <atomic>
#include <cassert>
#include <chrono>
#include <type_traits>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <optional>

#ifdef __cpp_exceptions
#    include <stdexcept>
#endif

namespace zoox
{

// Forward declarations
template <typename ReferenceType,
          typename BaseType                   = ReferenceType,
          template <typename> class OptionalT = std::optional,
          typename Deleter                    = std::default_delete<BaseType>>
class unique_reference;

template <typename T,
          template <typename> class OptionalT = std::optional,
          typename Deleter                    = std::default_delete<T>>
class waitable_ref_owner;

#ifdef __cpp_exceptions
// Exception thrown when attempting to create a unique_reference
// from a ref_owner that has been marked for deletion
class ref_owner_marked_exception : public std::runtime_error
{
public:
    ref_owner_marked_exception()
        : std::runtime_error("Cannot create unique_reference: "
                             "ref_owner is marked for deletion")
    {
    }
};
#endif

// =============================================================================
// ref_owner - Lock-free base implementation
// =============================================================================
//
// A smart pointer with explicit deletion control. References can be created
// and destroyed lock-free. Deletion only occurs when explicitly requested
// AND no outstanding references exist.
//
// For blocking wait functionality, use waitable_ref_owner wrapper.
//
// TLA+ SPECIFICATION CORRESPONDENCE (specs/UniqueReference.tla):
// =============================================================================
//
// VARIABLES (TLA+ -> C++):
//   refCount           -> ref_count_            (atomic<size_t>)
//   markedForDeletion  -> marked_for_deletion_  (atomic<bool>)
//   deleted            -> deleted_              (atomic<bool>)
//   clientRefs[c]      -> (implicit in unique_reference instances)
//
// SAFETY INVARIANTS (proven by TLC model checker):
//   NoUseAfterFree:        deleted => (refCount = 0)
//   NoInvalidReference:    ~(deleted /\ refCount > 0)
//   ReferencesAlwaysValid: (refCount > 0) => ~deleted
//   DeletionImpliesMarked: deleted => markedForDeletion
//
// PROTOCOL: Owner must not destroy ref_owner while has_outstanding_references()
//
template <typename T,
          template <typename> class OptionalT = std::optional,
          typename Deleter                    = std::default_delete<T>>
class ref_owner
{
public:
    using deleter_type = Deleter;

    // Construction
    explicit ref_owner(T* ptr)
        : owned_ptr_(ptr)
    {
    }

    explicit ref_owner(T* ptr, Deleter d)
        : owned_ptr_(ptr, std::move(d))
    {
    }

    explicit ref_owner(std::unique_ptr<T, Deleter> ptr)
        : owned_ptr_(std::move(ptr))
    {
    }

    // Destructor
#ifdef NDEBUG
    ~ref_owner() noexcept(std::is_nothrow_destructible<T>::value)
    {
        delete_if_deleteable();
    }
#else
#    ifdef __cpp_exceptions
    ~ref_owner() noexcept(false)
    {
        if (deleted_.load(std::memory_order_acquire))
        {
            return;
        }
        if (not delete_if_deleteable())
        {
            throw std::logic_error("ref_owner destroyed with outstanding references");
        }
    }
#    else
    ~ref_owner() noexcept
    {
        if (deleted_.load(std::memory_order_acquire))
        {
            return;
        }
        assert(delete_if_deleteable() && "ref_owner destroyed with outstanding references");
    }
#    endif
#endif

    // Non-copyable
    ref_owner(const ref_owner&)            = delete;
    ref_owner& operator=(const ref_owner&) = delete;

    // Movable
    ref_owner(ref_owner&& other) noexcept
        : owned_ptr_(std::move(other.owned_ptr_))
        , ref_count_(other.ref_count_.load(std::memory_order_relaxed))
        , marked_for_deletion_(other.marked_for_deletion_.load(std::memory_order_relaxed))
        , deleted_(other.deleted_.load(std::memory_order_relaxed))
    {
        other.ref_count_.store(0, std::memory_order_relaxed);
    }

    ref_owner& operator=(ref_owner&& other) noexcept(std::is_nothrow_destructible<T>::value)
    {
        if (this != &other)
        {
            owned_ptr_ = std::move(other.owned_ptr_);
            ref_count_.store(other.ref_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            marked_for_deletion_.store(other.marked_for_deletion_.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
            deleted_.store(other.deleted_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.ref_count_.store(0, std::memory_order_relaxed);
        }
        return *this;
    }

    // Standard smart pointer interface
    T* get() const noexcept
    {
        return owned_ptr_.get();
    }
    T& operator*() const
    {
        return *owned_ptr_;
    }
    T* operator->() const noexcept
    {
        return owned_ptr_.get();
    }
    explicit operator bool() const noexcept
    {
        return owned_ptr_ != nullptr;
    }

    // Reference creation - returns OptionalT (default: std::optional)
    // Returns empty optional if marked for deletion
    // LOCK-FREE: Uses optimistic increment + check + rollback pattern
    OptionalT<unique_reference<T, T, OptionalT, Deleter>> try_make_ref() noexcept
    {
        if (!try_register_ref())
        {
            return {};  // Default construction = empty optional
        }
        typename unique_reference<T, T, OptionalT, Deleter>::already_registered tag;
        return OptionalT<unique_reference<T, T, OptionalT, Deleter>>(
            unique_reference<T, T, OptionalT, Deleter>(*this, tag));
    }

#ifdef __cpp_exceptions
    // Reference creation - throws if marked for deletion
    unique_reference<T, T, OptionalT, Deleter> make_ref()
    {
        if (!try_register_ref())
        {
            throw ref_owner_marked_exception();
        }
        typename unique_reference<T, T, OptionalT, Deleter>::already_registered tag;
        return unique_reference<T, T, OptionalT, Deleter>(*this, tag);
    }
#endif

    // Query methods
    bool has_outstanding_references() const noexcept
    {
        return ref_count_.load(std::memory_order_acquire) > 0;
    }

    size_t ref_count() const noexcept
    {
        return ref_count_.load(std::memory_order_acquire);
    }

    bool is_marked_for_deletion() const noexcept
    {
        return marked_for_deletion_.load(std::memory_order_acquire);
    }

    bool is_deleted() const noexcept
    {
        return deleted_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // TLA+ SPEC: MarkForDeletion
    // =========================================================================
    // MarkForDeletion ==
    //     /\ ~markedForDeletion           (* Precondition: not marked *)
    //     /\ ~deleted                      (* Precondition: not deleted *)
    //     /\ markedForDeletion' = TRUE    (* Action: set flag *)
    //     /\ UNCHANGED <<refCount, deleted, clientRefs>>
    // =========================================================================
    // Mark for deletion (lock-free, non-blocking)
    // After this, no new references can be created (TryMakeRefFail will occur)
    void mark_for_deletion() noexcept
    {
        // SPEC: markedForDeletion' = TRUE
        marked_for_deletion_.store(true, std::memory_order_seq_cst);
    }

    // =========================================================================
    // TLA+ SPEC: DeleteIfDeleteable
    // =========================================================================
    // DeleteIfDeleteable ==
    //     /\ markedForDeletion            (* Precondition: must be marked *)
    //     /\ ~deleted                      (* Precondition: not already deleted *)
    //     /\ refCount = 0                  (* PROTOCOL: no outstanding refs *)
    //     /\ deleted' = TRUE              (* Action: mark as deleted *)
    //     /\ UNCHANGED <<refCount, markedForDeletion, clientRefs>>
    //
    // SAFETY: This enforces NoInvalidReference: ~(deleted /\ refCount > 0)
    // =========================================================================
    // Try to delete if conditions are met (lock-free, non-blocking)
    // Returns true if deletion occurred, false otherwise
    bool delete_if_deleteable() noexcept(std::is_nothrow_destructible<T>::value)
    {
        // SPEC: Precondition markedForDeletion
        if (!marked_for_deletion_.load(std::memory_order_acquire))
        {
            return false;
        }

        // SPEC: Precondition ~deleted
        if (deleted_.load(std::memory_order_acquire))
        {
            return false;
        }

        // SPEC: PROTOCOL refCount = 0 (enforces NoInvalidReference)
        if (ref_count_.load(std::memory_order_acquire) != 0)
        {
            return false;
        }

        // SPEC: deleted' = TRUE (atomic CAS for thread safety)
        bool expected = false;
        if (deleted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            owned_ptr_.reset();
            return true;
        }

        return false;
    }

    // Convenience: mark and try to delete in one call
    bool mark_and_delete_if_ready() noexcept(std::is_nothrow_destructible<T>::value)
    {
        mark_for_deletion();
        return delete_if_deleteable();
    }

protected:
    template <typename RefType, typename BaseType, template <typename> class Opt, typename Del>
    friend class unique_reference;
    friend class waitable_ref_owner<T, OptionalT, Deleter>;

    // =========================================================================
    // TLA+ SPEC: TryMakeRefSuccess(c) / TryMakeRefFail(c)
    // =========================================================================
    // TryMakeRefSuccess(c) ==
    //     /\ ~markedForDeletion           (* Precondition: not marked *)
    //     /\ ~deleted                      (* Precondition: not deleted *)
    //     /\ refCount' = refCount + 1      (* Action: increment *)
    //     /\ clientRefs' = [clientRefs EXCEPT ![c] = @ + 1]
    //     /\ UNCHANGED <<markedForDeletion, deleted>>
    //
    // TryMakeRefFail(c) ==
    //     /\ markedForDeletion            (* Precondition: already marked *)
    //     /\ UNCHANGED vars               (* No state change - rollback *)
    // =========================================================================
    // Core atomic logic for ref registration - generated once per ref_owner<T>
    // Returns true if ref was successfully registered, false if marked for deletion
    // LOCK-FREE: Uses optimistic increment + check + rollback pattern
    bool try_register_ref() noexcept
    {
        // SPEC: refCount' = refCount + 1 (optimistic increment FIRST)
        ref_count_.fetch_add(1, std::memory_order_seq_cst);

        // SPEC: Check ~markedForDeletion (if true -> TryMakeRefFail)
        if (marked_for_deletion_.load(std::memory_order_seq_cst))
        {
            // SPEC: TryMakeRefFail - rollback, UNCHANGED vars
            ref_count_.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        // SPEC: TryMakeRefSuccess - ref registered
        return true;
    }

    // =========================================================================
    // TLA+ SPEC: ReleaseRef(c)
    // =========================================================================
    // ReleaseRef(c) ==
    //     /\ clientRefs[c] > 0            (* Precondition: has ref *)
    //     /\ refCount' = refCount - 1     (* Action: decrement *)
    //     /\ clientRefs' = [clientRefs EXCEPT ![c] = @ - 1]
    //     /\ UNCHANGED <<markedForDeletion, deleted>>
    // =========================================================================
    // Called by unique_reference destructor
    virtual void on_ref_released() noexcept
    {
        // SPEC: refCount' = refCount - 1
        ref_count_.fetch_sub(1, std::memory_order_seq_cst);
    }

    std::unique_ptr<T, Deleter> owned_ptr_;
    // TLA+ SPEC VARIABLE: refCount (Int, 0..MaxRefs)
    std::atomic<size_t> ref_count_{0};
    // TLA+ SPEC VARIABLE: markedForDeletion (Bool)
    std::atomic<bool> marked_for_deletion_{false};
    // TLA+ SPEC VARIABLE: deleted (Bool)
    std::atomic<bool> deleted_{false};
};

// =============================================================================
// unique_reference - Reference to a ref_owner's managed object
// =============================================================================
//
// Similar to std::reference_wrapper but with unique (non-copyable) ownership
// semantics. get() returns T& instead of T*.
//
// TLA+ SPECIFICATION CORRESPONDENCE (specs/UniqueReference.tla):
// =============================================================================
//
// Each unique_reference instance represents a client holding a reference:
//   clientRefs[c] > 0    <->  unique_reference exists for client c
//
// LIFECYCLE:
//   Construction: TryMakeRefSuccess(c) - refCount++, clientRefs[c]++
//   Destruction:  ReleaseRef(c)        - refCount--, clientRefs[c]--
//
// SAFETY GUARANTEE (proven by TLC):
//   ReferencesAlwaysValid: (clientRefs[c] > 0) => ~deleted
//   i.e., if this reference exists, the object is not deleted
//

template <typename ReferenceType, typename BaseType, template <typename> class OptionalT, typename Deleter>
class unique_reference
{
public:
    using type = ReferenceType;

    // Tag for internal use - ref is already registered
    struct already_registered
    {};

    // Internal construction - ref already registered by make_ref()
    // SPEC: Called after TryMakeRefSuccess, clientRefs[c] already incremented
    unique_reference(ref_owner<BaseType, OptionalT, Deleter>& owner, already_registered)
        : owner_(&owner)
    {
    }

    // SPEC: ReleaseRef(c) - decrement refCount and clientRefs[c]
    ~unique_reference() noexcept
    {
        if (owner_)
        {
            // SPEC: ReleaseRef - on_ref_released() decrements refCount
            owner_->on_ref_released();
        }
    }

    // Non-copyable (unique ownership)
    unique_reference(const unique_reference&)            = delete;
    unique_reference& operator=(const unique_reference&) = delete;

    // Move-constructible (transfers ownership, source won't decrement ref count)
    unique_reference(unique_reference&& other) noexcept
        : owner_(other.owner_)
    {
        other.owner_ = nullptr;
    }

    // Converting move constructor - enables static upcasts
    // Only enabled when OtherRef* is implicitly convertible to ReferenceType*
    template <typename OtherRef,
              typename = std::enable_if_t<std::is_convertible_v<OtherRef*, ReferenceType*> &&
                                          !std::is_same_v<OtherRef, ReferenceType>>>
    unique_reference(unique_reference<OtherRef, BaseType, OptionalT, Deleter>&& other) noexcept
        : owner_(other.owner_)
    {
        other.owner_ = nullptr;
    }

    // Non-move-assignable (keep it simple)
    unique_reference& operator=(unique_reference&&) = delete;

    // Core access - like std::reference_wrapper
    // Returns ReferenceType& by casting from BaseType&
    ReferenceType& get() const noexcept
    {
        return static_cast<ReferenceType&>(*owner_->get());
    }

    // Implicit conversion - like std::reference_wrapper
    operator ReferenceType&() const noexcept
    {
        return get();
    }

    // Pointer-like access (bonus over reference_wrapper)
    ReferenceType& operator*() const noexcept
    {
        return get();
    }

    ReferenceType* operator->() const noexcept
    {
        return &get();
    }

    // Callable support - like std::reference_wrapper
    template <typename... Args>
    auto operator()(Args&&... args) const
        -> decltype(std::invoke(std::declval<ReferenceType&>(), std::forward<Args>(args)...))
    {
        return std::invoke(get(), std::forward<Args>(args)...);
    }

private:
    // Allow other unique_reference instantiations to access owner_ for converting moves
    template <typename OtherRef, typename OtherBase, template <typename> class OtherOptionalT, typename OtherDeleter>
    friend class unique_reference;

    // Allow dynamic_reference_move to construct with raw owner pointer
    template <typename U, typename RefT, typename Base, template <typename> class Opt, typename Del>
    friend Opt<unique_reference<U, Base, Opt, Del>> dynamic_reference_move(
        unique_reference<RefT, Base, Opt, Del>&& ref) noexcept;

    // Private constructor for dynamic_reference_move (bypasses SFINAE)
    explicit unique_reference(ref_owner<BaseType, OptionalT, Deleter>* owner_ptr) noexcept
        : owner_(owner_ptr)
    {
    }
    ref_owner<BaseType, OptionalT, Deleter>* owner_;
};

// =============================================================================
// Reference cast functions for unique_reference
// =============================================================================
//
// These enable type-safe casting of unique_reference with move semantics.
// The source reference is moved-from after a successful cast.
//
// Usage:
//   unique_reference<Derived> derived_ref = ...;
//   auto base_ref = zoox::static_reference_move<Base>(std::move(derived_ref));
//   // derived_ref is now moved-from, base_ref owns the reference
//

// Static reference move - compile-time validated upcast
// Moves the reference and changes the ReferenceType while preserving BaseType
// Only valid when T* is implicitly convertible to U* (e.g., derived-to-base)
template <typename U, typename T, typename Base, template <typename> class OptionalT, typename Deleter>
unique_reference<U, Base, OptionalT, Deleter> static_reference_move(
    unique_reference<T, Base, OptionalT, Deleter>&& ref) noexcept
{
    static_assert(std::is_convertible_v<T*, U*>, "static_reference_move: T* must be implicitly convertible to U*");
    return unique_reference<U, Base, OptionalT, Deleter>(std::move(ref));
}

// Dynamic reference move - runtime-checked cast
// Returns OptionalT containing the cast reference, or empty if cast fails
// On failure, the source reference remains valid (not moved-from)
template <typename U, typename T, typename Base, template <typename> class OptionalT, typename Deleter>
OptionalT<unique_reference<U, Base, OptionalT, Deleter>> dynamic_reference_move(
    unique_reference<T, Base, OptionalT, Deleter>&& ref) noexcept
{
    U* cast = dynamic_cast<U*>(&ref.get());
    if (!cast)
    {
        return {};  // Default construction = empty optional
    }
    // Use private constructor and transfer ownership
    unique_reference<U, Base, OptionalT, Deleter> result(ref.owner_);
    ref.owner_ = nullptr;
    return result;
}

// =============================================================================
// waitable_ref_owner - Wrapper adding efficient blocking wait
// =============================================================================
//
// Wraps ref_owner to add blocking wait functionality using OS primitives.
// Use this when you need to wait for all references to be released.
//
template <typename T, template <typename> class OptionalT, typename Deleter>
class waitable_ref_owner : public ref_owner<T, OptionalT, Deleter>
{
public:
    using base = ref_owner<T, OptionalT, Deleter>;

    // Construction - forwards to base
    explicit waitable_ref_owner(T* ptr)
        : base(ptr)
    {
    }

    explicit waitable_ref_owner(T* ptr, Deleter d)
        : base(ptr, std::move(d))
    {
    }

    explicit waitable_ref_owner(std::unique_ptr<T, Deleter> ptr)
        : base(std::move(ptr))
    {
    }

    // Non-copyable
    waitable_ref_owner(const waitable_ref_owner&)            = delete;
    waitable_ref_owner& operator=(const waitable_ref_owner&) = delete;

    // Movable (but be careful - don't move while waiting!)
    waitable_ref_owner(waitable_ref_owner&&)            = default;
    waitable_ref_owner& operator=(waitable_ref_owner&&) = default;

    // Wait indefinitely for all refs to be released, then delete
    void mark_and_wait_for_deletion()
    {
        base::mark_for_deletion();

        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait(lock, [this]() { return base::ref_count_.load(std::memory_order_acquire) == 0; });

        // All refs released, delete the object
        base::owned_ptr_.reset();
        base::deleted_.store(true, std::memory_order_release);
    }

    // Wait with timeout for all refs to be released
    // Returns true if deletion occurred, false if timeout
    bool mark_and_wait_for_deletion(std::chrono::milliseconds timeout)
    {
        base::mark_for_deletion();

        std::unique_lock<std::mutex> lock(wait_mutex_);
        bool                         completed = wait_cv_.wait_for(lock, timeout, [this]() {
            return base::ref_count_.load(std::memory_order_acquire) == 0;
        });

        if (completed)
        {
            base::owned_ptr_.reset();
            base::deleted_.store(true, std::memory_order_release);
        }
        return completed;
    }

    // Wait with deadline
    template <typename Clock, typename Duration>
    bool mark_and_wait_until_deletion(std::chrono::time_point<Clock, Duration> deadline)
    {
        base::mark_for_deletion();

        std::unique_lock<std::mutex> lock(wait_mutex_);
        bool                         completed = wait_cv_.wait_until(lock, deadline, [this]() {
            return base::ref_count_.load(std::memory_order_acquire) == 0;
        });

        if (completed)
        {
            base::owned_ptr_.reset();
            base::deleted_.store(true, std::memory_order_release);
        }
        return completed;
    }

protected:
    // Override to notify waiters when ref count changes
    void on_ref_released() noexcept override
    {
        size_t prev = base::ref_count_.fetch_sub(1, std::memory_order_seq_cst);

        // If this was the last ref and we're marked for deletion, notify waiters
        if (prev == 1 && base::marked_for_deletion_.load(std::memory_order_acquire))
        {
            // Lock to synchronize with wait_cv_.wait()
            std::lock_guard<std::mutex> lock(wait_mutex_);
            wait_cv_.notify_all();
        }
    }

private:
    std::mutex              wait_mutex_;
    std::condition_variable wait_cv_;
};

}  // namespace zoox

#endif  // ZOOX_MEMORY_W_UNIQUE_REFERENCE_H
