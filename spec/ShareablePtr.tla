--------------------------- MODULE ShareablePtr ---------------------------
(*
 * TLA+ specification for ShareablePtr - a thread-safe reference-counted
 * smart pointer that supports sharing ownership across threads.
 *
 * This spec models the key invariants:
 * - Reference count is always >= 0
 * - Object is destroyed exactly when refcount reaches 0
 * - Multiple threads can safely acquire/release references
 *)

EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    \* @type: Int;
    MaxThreads,     \* Maximum number of concurrent threads
    \* @type: Int;
    MaxRefCount     \* Maximum reference count (for bounded model checking)

VARIABLES
    \* @type: Int;
    refCount,       \* Current reference count
    \* @type: Bool;
    objectAlive,    \* Whether the managed object exists
    \* @type: Set(Int);
    threadRefs      \* Set of threads currently holding a reference

vars == <<refCount, objectAlive, threadRefs>>

-----------------------------------------------------------------------------
(* Constant initialization for Apalache *)
CInit ==
    /\ MaxThreads = 4
    /\ MaxRefCount = 8

TypeInvariant ==
    /\ refCount \in 0..MaxRefCount
    /\ objectAlive \in BOOLEAN
    /\ threadRefs \subseteq 1..MaxThreads

-----------------------------------------------------------------------------
(* Safety Invariants *)

\* Reference count matches number of threads holding references
RefCountConsistent ==
    refCount = Cardinality(threadRefs)

\* Object is alive iff refcount > 0
ObjectLifetimeCorrect ==
    objectAlive <=> (refCount > 0)

\* No thread holds a reference to a dead object
NoUseAfterFree ==
    ~objectAlive => threadRefs = {}

SafetyInvariant ==
    /\ TypeInvariant
    /\ RefCountConsistent
    /\ ObjectLifetimeCorrect
    /\ NoUseAfterFree

-----------------------------------------------------------------------------
(* Initial State *)

Init ==
    /\ refCount = 1              \* Created with initial reference
    /\ objectAlive = TRUE
    /\ threadRefs = {1}          \* Thread 1 is the creator

-----------------------------------------------------------------------------
(* Actions *)

\* A thread acquires a new reference (copy constructor / share())
Acquire(t) ==
    /\ objectAlive                           \* Can only acquire if alive
    /\ t \notin threadRefs                   \* Thread doesn't already have ref
    /\ refCount < MaxRefCount                \* Bounded for model checking
    /\ refCount' = refCount + 1
    /\ threadRefs' = threadRefs \cup {t}
    /\ UNCHANGED objectAlive

\* A thread releases its reference (destructor / reset())
Release(t) ==
    /\ t \in threadRefs                      \* Thread must have a reference
    /\ refCount' = refCount - 1
    /\ threadRefs' = threadRefs \ {t}
    /\ objectAlive' = (refCount' > 0)        \* Destroy if last reference

\* Thread acquires reference from another thread (move semantics)
Move(from, to) ==
    /\ from \in threadRefs                   \* Source has reference
    /\ to \notin threadRefs                  \* Dest doesn't have reference
    /\ from # to                             \* Different threads
    /\ threadRefs' = (threadRefs \ {from}) \cup {to}
    /\ UNCHANGED <<refCount, objectAlive>>   \* Move doesn't change refcount

-----------------------------------------------------------------------------
(* Next State Relation *)

Next ==
    \/ \E t \in 1..MaxThreads : Acquire(t)
    \/ \E t \in 1..MaxThreads : Release(t)
    \/ \E from, to \in 1..MaxThreads : Move(from, to)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Liveness Properties *)

\* Eventually the object can be destroyed (no memory leak)
EventuallyFreed == <>(refCount = 0)

\* Fairness: if a thread can release, it eventually will
FairSpec == Spec /\ WF_vars(Next)

=============================================================================
