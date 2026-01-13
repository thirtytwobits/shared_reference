// Copyright (c) Zoox.
// SPDX-License-Identifier: MIT

// =============================================================================
// TLA+ Specification-Derived Tests for ref_owner
// =============================================================================
//
// These tests are derived from specs/UniqueReference.tla to verify the C++
// implementation matches the formal specification. Each test corresponds to
// a TLA+ action or invariant.
//
// SPECIFICATION CORRESPONDENCE:
//   TLA+ File:    specs/UniqueReference.tla
//   C++ File:     include/zoox/memory_w_unique_reference.h
//
// Run these tests with TSAN to catch data races:
//   bazel test --config=tsan //:ref_owner_tla_test
//

#include <gtest/gtest.h>
#include <zoox/memory_w_unique_reference.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace zoox
{
namespace
{

// Test object that tracks construction/destruction
struct TrackedObject
{
    static std::atomic<int> live_count;
    static std::atomic<int> total_constructed;

    int value;

    explicit TrackedObject(int v = 0) : value(v)
    {
        ++live_count;
        ++total_constructed;
    }

    ~TrackedObject()
    {
        --live_count;
    }

    TrackedObject(const TrackedObject&)            = delete;
    TrackedObject& operator=(const TrackedObject&) = delete;
};

std::atomic<int> TrackedObject::live_count{0};
std::atomic<int> TrackedObject::total_constructed{0};

class TlaSpecTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TrackedObject::live_count          = 0;
        TrackedObject::total_constructed   = 0;
    }

    void TearDown() override
    {
        // Verify no leaks after each test
        EXPECT_EQ(TrackedObject::live_count.load(), 0) << "Memory leak detected";
    }
};

// =============================================================================
// TLA+ Init State Tests
// =============================================================================
// Init ==
//     /\ refCount = 0
//     /\ markedForDeletion = FALSE
//     /\ deleted = FALSE
//     /\ clientRefs = [c \in Clients |-> 0]

TEST_F(TlaSpecTest, Init_InitialState)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // SPEC: refCount = 0
    EXPECT_EQ(ptr.ref_count(), 0);

    // SPEC: markedForDeletion = FALSE
    EXPECT_FALSE(ptr.is_marked_for_deletion());

    // SPEC: deleted = FALSE
    EXPECT_FALSE(ptr.is_deleted());

    // SPEC: Object is alive
    EXPECT_EQ(TrackedObject::live_count.load(), 1);

    // Cleanup
    ptr.mark_for_deletion();
}

// =============================================================================
// TLA+ TryMakeRefSuccess Tests
// =============================================================================
// TryMakeRefSuccess(c) ==
//     /\ ~markedForDeletion           (* Precondition: not marked *)
//     /\ ~deleted                      (* Precondition: not deleted *)
//     /\ refCount' = refCount + 1      (* Action: increment *)
//     /\ clientRefs' = [clientRefs EXCEPT ![c] = @ + 1]
//     /\ UNCHANGED <<markedForDeletion, deleted>>

TEST_F(TlaSpecTest, TryMakeRefSuccess_PreconditionsAndPostconditions)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // SPEC: Precondition ~markedForDeletion
    ASSERT_FALSE(ptr.is_marked_for_deletion());
    // SPEC: Precondition ~deleted
    ASSERT_FALSE(ptr.is_deleted());

    size_t initial_ref_count = ptr.ref_count();

    // Action: TryMakeRefSuccess
    auto ref = ptr.try_make_ref();

    // SPEC: Reference acquired
    EXPECT_TRUE(ref.has_value());

    // SPEC: refCount' = refCount + 1
    EXPECT_EQ(ptr.ref_count(), initial_ref_count + 1);

    // SPEC: UNCHANGED markedForDeletion
    EXPECT_FALSE(ptr.is_marked_for_deletion());

    // SPEC: UNCHANGED deleted
    EXPECT_FALSE(ptr.is_deleted());

    // Cleanup
    ref.reset();
    ptr.mark_for_deletion();
}

TEST_F(TlaSpecTest, TryMakeRefSuccess_MultipleRefs)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Create multiple refs
    auto ref1 = ptr.try_make_ref();
    auto ref2 = ptr.try_make_ref();
    auto ref3 = ptr.try_make_ref();

    // SPEC: Each TryMakeRefSuccess increments refCount
    EXPECT_EQ(ptr.ref_count(), 3);

    // All refs valid
    EXPECT_TRUE(ref1.has_value());
    EXPECT_TRUE(ref2.has_value());
    EXPECT_TRUE(ref3.has_value());

    // Cleanup
    ref1.reset();
    ref2.reset();
    ref3.reset();
    ptr.mark_for_deletion();
}

// =============================================================================
// TLA+ TryMakeRefFail Tests
// =============================================================================
// TryMakeRefFail(c) ==
//     /\ markedForDeletion            (* Precondition: already marked *)
//     /\ UNCHANGED vars               (* No state change - rollback *)

TEST_F(TlaSpecTest, TryMakeRefFail_WhenMarked)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Setup: mark for deletion
    ptr.mark_for_deletion();

    // SPEC: Precondition markedForDeletion
    ASSERT_TRUE(ptr.is_marked_for_deletion());

    size_t ref_count_before = ptr.ref_count();

    // Action: TryMakeRefFail
    auto ref = ptr.try_make_ref();

    // SPEC: Reference NOT acquired
    EXPECT_FALSE(ref.has_value());

    // SPEC: UNCHANGED vars (rollback occurred)
    EXPECT_EQ(ptr.ref_count(), ref_count_before);
}

TEST_F(TlaSpecTest, TryMakeRefFail_MultipleAttempts)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));
    ptr.mark_for_deletion();

    // Multiple attempts should all fail
    for (int i = 0; i < 10; ++i)
    {
        auto ref = ptr.try_make_ref();
        EXPECT_FALSE(ref.has_value());
    }

    // SPEC: refCount unchanged (all rollbacks succeeded)
    EXPECT_EQ(ptr.ref_count(), 0);
}

// =============================================================================
// TLA+ ReleaseRef Tests
// =============================================================================
// ReleaseRef(c) ==
//     /\ clientRefs[c] > 0            (* Precondition: has ref *)
//     /\ refCount' = refCount - 1     (* Action: decrement *)
//     /\ clientRefs' = [clientRefs EXCEPT ![c] = @ - 1]
//     /\ UNCHANGED <<markedForDeletion, deleted>>

TEST_F(TlaSpecTest, ReleaseRef_Decrement)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    {
        auto ref = ptr.try_make_ref();
        ASSERT_TRUE(ref.has_value());
        EXPECT_EQ(ptr.ref_count(), 1);

        // SPEC: Precondition clientRefs[c] > 0 (we have ref)
    }  // ref destroyed here - ReleaseRef

    // SPEC: refCount' = refCount - 1
    EXPECT_EQ(ptr.ref_count(), 0);

    // SPEC: UNCHANGED markedForDeletion
    EXPECT_FALSE(ptr.is_marked_for_deletion());

    // SPEC: UNCHANGED deleted
    EXPECT_FALSE(ptr.is_deleted());

    ptr.mark_for_deletion();
}

TEST_F(TlaSpecTest, ReleaseRef_MultipleReleases)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    {
        auto ref1 = ptr.try_make_ref();
        auto ref2 = ptr.try_make_ref();
        auto ref3 = ptr.try_make_ref();
        EXPECT_EQ(ptr.ref_count(), 3);
    }  // All refs destroyed - 3 ReleaseRef actions

    EXPECT_EQ(ptr.ref_count(), 0);
    ptr.mark_for_deletion();
}

// =============================================================================
// TLA+ MarkForDeletion Tests
// =============================================================================
// MarkForDeletion ==
//     /\ ~markedForDeletion           (* Precondition: not marked *)
//     /\ ~deleted                      (* Precondition: not deleted *)
//     /\ markedForDeletion' = TRUE    (* Action: set flag *)
//     /\ UNCHANGED <<refCount, deleted, clientRefs>>

TEST_F(TlaSpecTest, MarkForDeletion_Basic)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // SPEC: Precondition ~markedForDeletion
    ASSERT_FALSE(ptr.is_marked_for_deletion());
    // SPEC: Precondition ~deleted
    ASSERT_FALSE(ptr.is_deleted());

    size_t ref_count_before = ptr.ref_count();

    // Action: MarkForDeletion
    ptr.mark_for_deletion();

    // SPEC: markedForDeletion' = TRUE
    EXPECT_TRUE(ptr.is_marked_for_deletion());

    // SPEC: UNCHANGED refCount
    EXPECT_EQ(ptr.ref_count(), ref_count_before);

    // SPEC: UNCHANGED deleted
    EXPECT_FALSE(ptr.is_deleted());
}

TEST_F(TlaSpecTest, MarkForDeletion_IdempotentCalls)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Multiple marks should be safe (idempotent)
    ptr.mark_for_deletion();
    ptr.mark_for_deletion();
    ptr.mark_for_deletion();

    EXPECT_TRUE(ptr.is_marked_for_deletion());
}

// =============================================================================
// TLA+ DeleteIfDeleteable Tests
// =============================================================================
// DeleteIfDeleteable ==
//     /\ markedForDeletion            (* Precondition: must be marked *)
//     /\ ~deleted                      (* Precondition: not already deleted *)
//     /\ refCount = 0                  (* PROTOCOL: no outstanding refs *)
//     /\ deleted' = TRUE              (* Action: mark as deleted *)
//     /\ UNCHANGED <<refCount, markedForDeletion, clientRefs>>

TEST_F(TlaSpecTest, DeleteIfDeleteable_Success)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    ptr.mark_for_deletion();

    // SPEC: Precondition markedForDeletion
    ASSERT_TRUE(ptr.is_marked_for_deletion());
    // SPEC: Precondition ~deleted
    ASSERT_FALSE(ptr.is_deleted());
    // SPEC: Precondition refCount = 0
    ASSERT_EQ(ptr.ref_count(), 0);

    // Action: DeleteIfDeleteable
    bool deleted = ptr.delete_if_deleteable();

    EXPECT_TRUE(deleted);
    // SPEC: deleted' = TRUE
    EXPECT_TRUE(ptr.is_deleted());
    // Object destroyed
    EXPECT_EQ(TrackedObject::live_count.load(), 0);
}

TEST_F(TlaSpecTest, DeleteIfDeleteable_FailsIfNotMarked)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // SPEC: Precondition violated - not marked
    ASSERT_FALSE(ptr.is_marked_for_deletion());

    bool deleted = ptr.delete_if_deleteable();

    EXPECT_FALSE(deleted);
    EXPECT_FALSE(ptr.is_deleted());
    // Object still alive
    EXPECT_EQ(TrackedObject::live_count.load(), 1);

    ptr.mark_for_deletion();
}

TEST_F(TlaSpecTest, DeleteIfDeleteable_FailsWithOutstandingRefs)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    auto ref = ptr.try_make_ref();
    ptr.mark_for_deletion();

    // SPEC: Precondition violated - refCount > 0
    ASSERT_GT(ptr.ref_count(), 0);

    bool deleted = ptr.delete_if_deleteable();

    EXPECT_FALSE(deleted);
    EXPECT_FALSE(ptr.is_deleted());
    // Object still alive
    EXPECT_EQ(TrackedObject::live_count.load(), 1);

    ref.reset();
}

TEST_F(TlaSpecTest, DeleteIfDeleteable_IdempotentAfterDelete)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    ptr.mark_for_deletion();
    ptr.delete_if_deleteable();

    // Multiple delete calls should be safe
    EXPECT_FALSE(ptr.delete_if_deleteable());
    EXPECT_FALSE(ptr.delete_if_deleteable());

    EXPECT_TRUE(ptr.is_deleted());
}

// =============================================================================
// TLA+ Safety Invariant Tests
// =============================================================================

// NoUseAfterFree: deleted => (\A c \in Clients : clientRefs[c] = 0)
// Equivalently: if deleted, no client has refs
TEST_F(TlaSpecTest, SafetyInvariant_NoUseAfterFree)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Create a ref, mark, then verify we can't delete until ref released
    auto ref = ptr.try_make_ref();
    ASSERT_TRUE(ref.has_value());

    ptr.mark_for_deletion();

    // Cannot delete while refs exist (protocol enforced)
    EXPECT_FALSE(ptr.delete_if_deleteable());
    EXPECT_FALSE(ptr.is_deleted());

    // Release ref
    ref.reset();

    // Now can delete
    EXPECT_TRUE(ptr.delete_if_deleteable());

    // SPEC: NoUseAfterFree - deleted => no refs
    EXPECT_TRUE(ptr.is_deleted());
    EXPECT_EQ(ptr.ref_count(), 0);
}

// NoInvalidReference: ~(deleted /\ refCount > 0)
TEST_F(TlaSpecTest, SafetyInvariant_NoInvalidReference)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Invariant should hold throughout lifecycle
    auto check_invariant = [&ptr]() {
        // SPEC: ~(deleted /\ refCount > 0)
        EXPECT_FALSE(ptr.is_deleted() && ptr.ref_count() > 0);
    };

    check_invariant();

    auto ref1 = ptr.try_make_ref();
    check_invariant();

    auto ref2 = ptr.try_make_ref();
    check_invariant();

    ptr.mark_for_deletion();
    check_invariant();

    ref1.reset();
    check_invariant();

    ref2.reset();
    check_invariant();

    ptr.delete_if_deleteable();
    check_invariant();
}

// DeletionImpliesMarked: deleted => markedForDeletion
TEST_F(TlaSpecTest, SafetyInvariant_DeletionImpliesMarked)
{
    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Initially: not deleted, not marked
    EXPECT_FALSE(ptr.is_deleted());

    ptr.mark_for_deletion();
    ptr.delete_if_deleteable();

    // SPEC: deleted => markedForDeletion
    if (ptr.is_deleted())
    {
        EXPECT_TRUE(ptr.is_marked_for_deletion());
    }
}

// =============================================================================
// TLA+ Concurrent Action Tests (TSAN Critical)
// =============================================================================
// These tests exercise concurrent execution paths to catch data races.
// Run with: bazel test --config=tsan //:ref_owner_tla_test

TEST_F(TlaSpecTest, Concurrent_TryMakeRefSuccess)
{
    constexpr int NUM_THREADS = 8;
    constexpr int REFS_PER_THREAD = 100;

    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    std::atomic<int> successful_refs{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&ptr, &successful_refs]() {
            std::vector<std::optional<unique_reference<TrackedObject>>> refs;
            refs.reserve(REFS_PER_THREAD);

            for (int i = 0; i < REFS_PER_THREAD; ++i)
            {
                auto ref = ptr.try_make_ref();
                if (ref.has_value())
                {
                    ++successful_refs;
                    refs.push_back(std::move(ref));
                }
            }
            // refs released when vector destroyed
        });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    // All refs should have succeeded (not marked)
    EXPECT_EQ(successful_refs.load(), NUM_THREADS * REFS_PER_THREAD);

    // All refs released
    EXPECT_EQ(ptr.ref_count(), 0);

    ptr.mark_for_deletion();
}

TEST_F(TlaSpecTest, Concurrent_TryMakeRefFail)
{
    constexpr int NUM_THREADS = 8;
    constexpr int ATTEMPTS_PER_THREAD = 100;

    ref_owner<TrackedObject> ptr(new TrackedObject(42));
    ptr.mark_for_deletion();  // Mark before threads start

    std::atomic<int> failed_refs{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&ptr, &failed_refs]() {
            for (int i = 0; i < ATTEMPTS_PER_THREAD; ++i)
            {
                auto ref = ptr.try_make_ref();
                if (!ref.has_value())
                {
                    ++failed_refs;
                }
            }
        });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    // All refs should have failed
    EXPECT_EQ(failed_refs.load(), NUM_THREADS * ATTEMPTS_PER_THREAD);

    // SPEC: UNCHANGED vars - refCount should still be 0
    EXPECT_EQ(ptr.ref_count(), 0);
}

TEST_F(TlaSpecTest, Concurrent_MixedOperations)
{
    constexpr int NUM_CLIENTS = 4;
    constexpr int OPS_PER_CLIENT = 200;

    ref_owner<TrackedObject> ptr(new TrackedObject(42));

    std::atomic<bool> keep_running{true};
    std::atomic<int> total_successful_refs{0};
    std::vector<std::thread> clients;

    // Client threads: repeatedly acquire and release refs
    for (int c = 0; c < NUM_CLIENTS; ++c)
    {
        clients.emplace_back([&ptr, &keep_running, &total_successful_refs]() {
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, 100);

            int ops = 0;
            while (keep_running.load() && ops < OPS_PER_CLIENT)
            {
                auto ref = ptr.try_make_ref();
                if (ref.has_value())
                {
                    ++total_successful_refs;

                    // SPEC: ReferencesAlwaysValid - while we have ref, not deleted
                    EXPECT_FALSE(ptr.is_deleted());

                    // Hold ref for random time to increase interleaving
                    if (dist(rng) < 50)
                    {
                        std::this_thread::yield();
                    }
                }
                ++ops;
            }
        });
    }

    // Let clients run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Mark for deletion - new refs will fail
    ptr.mark_for_deletion();

    // Wait for clients to finish
    keep_running.store(false);
    for (auto& t : clients)
    {
        t.join();
    }

    // SPEC: All clients released their refs
    EXPECT_EQ(ptr.ref_count(), 0);

    // Now we can delete
    EXPECT_TRUE(ptr.delete_if_deleteable());
}

TEST_F(TlaSpecTest, Concurrent_DeleteIfDeleteable_Race)
{
    constexpr int NUM_DELETERS = 4;
    constexpr int ITERATIONS = 100;

    for (int iter = 0; iter < ITERATIONS; ++iter)
    {
        ref_owner<TrackedObject> ptr(new TrackedObject(iter));
        ptr.mark_for_deletion();

        std::atomic<int> delete_count{0};
        std::vector<std::thread> deleters;

        // Multiple threads try to delete concurrently
        for (int d = 0; d < NUM_DELETERS; ++d)
        {
            deleters.emplace_back([&ptr, &delete_count]() {
                if (ptr.delete_if_deleteable())
                {
                    ++delete_count;
                }
            });
        }

        for (auto& t : deleters)
        {
            t.join();
        }

        // SPEC: Only one deleter should succeed (CAS ensures this)
        EXPECT_EQ(delete_count.load(), 1) << "Iteration " << iter;

        // Object deleted
        EXPECT_TRUE(ptr.is_deleted());
    }
}

// =============================================================================
// TLA+ Protocol Compliance Tests
// =============================================================================
// These tests verify that the owner follows the protocol correctly

TEST_F(TlaSpecTest, Protocol_OwnerWaitsForRefs)
{
    waitable_ref_owner<TrackedObject> ptr(new TrackedObject(42));

    std::atomic<bool> ref_created{false};
    std::atomic<bool> ref_released{false};

    // Client thread: holds ref for a while
    std::thread client([&ptr, &ref_created, &ref_released]() {
        auto ref = ptr.try_make_ref();
        ASSERT_TRUE(ref.has_value());
        ref_created.store(true);

        // Hold ref
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ref_released.store(true);
        // ref released here
    });

    // Wait for ref to be created
    while (!ref_created.load())
    {
        std::this_thread::yield();
    }

    // Owner: mark and wait
    ptr.mark_and_wait_for_deletion();

    // Should only return after ref released
    EXPECT_TRUE(ref_released.load());
    EXPECT_TRUE(ptr.is_deleted());

    client.join();
}

TEST_F(TlaSpecTest, Protocol_TimeoutBehavior)
{
    waitable_ref_owner<TrackedObject> ptr(new TrackedObject(42));

    // Create ref that won't be released quickly
    auto ref = ptr.try_make_ref();
    ASSERT_TRUE(ref.has_value());

    // Owner: mark and wait with timeout
    bool deleted = ptr.mark_and_wait_for_deletion(std::chrono::milliseconds(10));

    // Should timeout - ref still held
    EXPECT_FALSE(deleted);
    EXPECT_FALSE(ptr.is_deleted());
    EXPECT_TRUE(ptr.is_marked_for_deletion());

    // Release ref
    ref.reset();

    // Now should succeed
    deleted = ptr.mark_and_wait_for_deletion(std::chrono::milliseconds(100));
    EXPECT_TRUE(deleted);
}

// =============================================================================
// TLA+ Stress Test
// =============================================================================
// High-contention test to find races under TSAN

TEST_F(TlaSpecTest, StressTest_HighContention)
{
    constexpr int NUM_ITERATIONS = 50;
    constexpr int NUM_CLIENTS = 8;
    constexpr int OPS_PER_CLIENT = 500;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter)
    {
        ref_owner<TrackedObject> ptr(new TrackedObject(iter));

        std::atomic<bool> stop{false};
        std::vector<std::thread> clients;

        // Spawn clients
        for (int c = 0; c < NUM_CLIENTS; ++c)
        {
            clients.emplace_back([&ptr, &stop]() {
                int ops = 0;
                while (!stop.load() && ops < OPS_PER_CLIENT)
                {
                    auto ref = ptr.try_make_ref();
                    if (ref.has_value())
                    {
                        // Verify invariant while holding ref
                        EXPECT_FALSE(ptr.is_deleted());
                    }
                    ++ops;
                }
            });
        }

        // Let run briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Mark for deletion
        ptr.mark_for_deletion();
        stop.store(true);

        // Wait for clients
        for (auto& t : clients)
        {
            t.join();
        }

        // All refs released
        EXPECT_EQ(ptr.ref_count(), 0);

        // Delete
        EXPECT_TRUE(ptr.delete_if_deleteable());
        EXPECT_TRUE(ptr.is_deleted());
    }
}

}  // namespace
}  // namespace zoox
