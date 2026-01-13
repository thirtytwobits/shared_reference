#include "zoox/memory_w_unique_reference.h"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <thread>

namespace zoox
{
namespace
{

// Test fixture with a simple test class
struct TestObject
{
    int                     value;
    static std::atomic<int> destruction_count;

    explicit TestObject(int v = 0)
        : value(v)
    {
    }
    ~TestObject()
    {
        destruction_count.fetch_add(1);
    }
};

std::atomic<int> TestObject::destruction_count{0};

class RefOwnerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestObject::destruction_count.store(0);
    }
};

// =============================================================================
// ref_owner Construction Tests
// =============================================================================

TEST_F(RefOwnerTest, ConstructFromRawPointer)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->value, 42);
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, ConstructFromUniquePtr)
{
    auto                      uptr = std::make_unique<TestObject>(99);
    ref_owner<TestObject> ptr(std::move(uptr));
    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->value, 99);
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, MoveConstruction)
{
    ref_owner<TestObject> ptr1(new TestObject(123));
    ref_owner<TestObject> ptr2(std::move(ptr1));

    EXPECT_TRUE(ptr2);
    EXPECT_EQ(ptr2->value, 123);
    EXPECT_FALSE(ptr1);  // NOLINT: testing moved-from state

    ptr2.mark_and_delete_if_ready();
    ptr1.mark_for_deletion();
}

TEST_F(RefOwnerTest, MoveAssignment)
{
    ref_owner<TestObject> ptr1(new TestObject(100));
    ref_owner<TestObject> ptr2(new TestObject(200));

    ptr2.mark_and_delete_if_ready();
    ptr2 = std::move(ptr1);

    EXPECT_TRUE(ptr2);
    EXPECT_EQ(ptr2->value, 100);
    EXPECT_FALSE(ptr1);  // NOLINT: testing moved-from state

    ptr2.mark_and_delete_if_ready();
    ptr1.mark_for_deletion();
}

// =============================================================================
// Smart Pointer Interface Tests
// =============================================================================

TEST_F(RefOwnerTest, GetReturnsRawPointer)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    TestObject*               raw = ptr.get();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->value, 42);
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, DereferenceOperator)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    EXPECT_EQ((*ptr).value, 42);
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, ArrowOperator)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    EXPECT_EQ(ptr->value, 42);
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, BoolConversionTrue)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    EXPECT_TRUE(static_cast<bool>(ptr));
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, BoolConversionFalseAfterMove)
{
    ref_owner<TestObject> ptr1(new TestObject(42));
    ref_owner<TestObject> ptr2(std::move(ptr1));
    EXPECT_FALSE(static_cast<bool>(ptr1));  // NOLINT
    ptr2.mark_and_delete_if_ready();
    ptr1.mark_for_deletion();
}

// =============================================================================
// Reference Creation Tests
// =============================================================================

TEST_F(RefOwnerTest, MakeRefCreatesValidReference)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        EXPECT_EQ(ref.get().value, 42);
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, MakeRefIncrementsRefCount)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    EXPECT_FALSE(ptr.has_outstanding_references());
    EXPECT_EQ(ptr.ref_count(), 0u);

    {
        auto ref = ptr.make_ref();
        EXPECT_TRUE(ptr.has_outstanding_references());
        EXPECT_EQ(ptr.ref_count(), 1u);
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, RefDestructionDecrementsRefCount)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        EXPECT_TRUE(ptr.has_outstanding_references());
    }
    EXPECT_FALSE(ptr.has_outstanding_references());
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, MultipleRefsAllValid)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref1 = ptr.make_ref();
        auto ref2 = ptr.make_ref();
        auto ref3 = ptr.make_ref();

        EXPECT_TRUE(ptr.has_outstanding_references());
        EXPECT_EQ(ptr.ref_count(), 3u);
        EXPECT_EQ(ref1.get().value, 42);
        EXPECT_EQ(ref2.get().value, 42);
        EXPECT_EQ(ref3.get().value, 42);
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, TryMakeRefReturnsNulloptAfterMarkedForDeletion)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    ptr.mark_for_deletion();

    auto ref = ptr.try_make_ref();
    EXPECT_FALSE(ref.has_value());
    ptr.delete_if_deleteable();
}

#ifdef __cpp_exceptions
TEST_F(RefOwnerTest, MakeRefThrowsAfterMarkedForDeletion)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    ptr.mark_for_deletion();

    EXPECT_THROW(ptr.make_ref(), ref_owner_marked_exception);
    ptr.delete_if_deleteable();
}
#endif

// =============================================================================
// Deletion Semantics Tests
// =============================================================================

TEST_F(RefOwnerTest, MarkForDeletion)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    EXPECT_FALSE(ptr.is_marked_for_deletion());

    ptr.mark_for_deletion();
    EXPECT_TRUE(ptr.is_marked_for_deletion());
    EXPECT_TRUE(ptr);  // Still holds object until deleted

    ptr.delete_if_deleteable();
}

TEST_F(RefOwnerTest, DeleteIfDeleteableWithNoRefs)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    ptr.mark_for_deletion();

    bool deleted = ptr.delete_if_deleteable();
    EXPECT_TRUE(deleted);
    EXPECT_TRUE(ptr.is_deleted());
    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

TEST_F(RefOwnerTest, DeleteIfDeleteableWithOutstandingRefs)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    auto ref = ptr.try_make_ref();
    ptr.mark_for_deletion();

    bool deleted = ptr.delete_if_deleteable();
    EXPECT_FALSE(deleted);
    EXPECT_FALSE(ptr.is_deleted());
    EXPECT_EQ(TestObject::destruction_count.load(), 0);

    ref.reset();
    ptr.delete_if_deleteable();
}

TEST_F(RefOwnerTest, DeleteIfDeleteableNotMarked)
{
    ref_owner<TestObject> ptr(new TestObject(42));

    bool deleted = ptr.delete_if_deleteable();
    EXPECT_FALSE(deleted);
    EXPECT_EQ(TestObject::destruction_count.load(), 0);

    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, MarkAndDeleteIfReady)
{
    ref_owner<TestObject> ptr(new TestObject(42));

    bool deleted = ptr.mark_and_delete_if_ready();
    EXPECT_TRUE(deleted);
    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

// =============================================================================
// waitable_ref_owner Tests
// =============================================================================

TEST_F(RefOwnerTest, WaitableMarkAndWaitForDeletionNoRefs)
{
    waitable_ref_owner<TestObject> ptr(new TestObject(42));
    ptr.mark_and_wait_for_deletion();

    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

TEST_F(RefOwnerTest, WaitableMarkAndWaitWithTimeoutSucceeds)
{
    waitable_ref_owner<TestObject> ptr(new TestObject(42));

    bool completed = ptr.mark_and_wait_for_deletion(std::chrono::milliseconds(100));
    EXPECT_TRUE(completed);
    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

TEST_F(RefOwnerTest, WaitableMarkAndWaitWithTimeoutTimesOut)
{
    waitable_ref_owner<TestObject> ptr(new TestObject(42));
    auto ref = ptr.try_make_ref();

    bool completed = ptr.mark_and_wait_for_deletion(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed);
    EXPECT_EQ(TestObject::destruction_count.load(), 0);

    ref.reset();
    ptr.delete_if_deleteable();
}

TEST_F(RefOwnerTest, WaitableMarkAndWaitWithConcurrentRefRelease)
{
    waitable_ref_owner<TestObject> ptr(new TestObject(42));
    auto ref = ptr.try_make_ref();

    std::thread waiter([&ptr]() { ptr.mark_and_wait_for_deletion(); });

    // Give the waiter time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Release the ref in another thread
    ref.reset();

    waiter.join();

    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

// =============================================================================
// unique_reference Tests
// =============================================================================

TEST_F(RefOwnerTest, RefGetReturnsReference)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        EXPECT_EQ(&ref.get(), ptr.get());
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, RefDereferenceOperator)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        EXPECT_EQ((*ref).value, 42);
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, RefArrowOperator)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        EXPECT_EQ(ref->value, 42);
    }
    ptr.mark_and_delete_if_ready();
}

TEST_F(RefOwnerTest, RefImplicitConversion)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    {
        auto ref = ptr.make_ref();
        TestObject& obj = ref;  // Implicit conversion to T&
        EXPECT_EQ(obj.value, 42);
    }
    ptr.mark_and_delete_if_ready();
}

// =============================================================================
// Concurrency Tests
// =============================================================================

TEST_F(RefOwnerTest, ConcurrentRefCreationAndDestruction)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    std::atomic<int>          successful_refs{0};

    constexpr int kNumThreads    = 8;
    constexpr int kRefsPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t)
    {
        threads.emplace_back([&ptr, &successful_refs]() {
            for (int i = 0; i < kRefsPerThread; ++i)
            {
                auto ref = ptr.try_make_ref();
                if (ref.has_value())
                {
                    EXPECT_EQ(ref->get().value, 42);
                    successful_refs.fetch_add(1);
                }
                // nullopt expected if marked during test
            }
        });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(successful_refs.load(), kNumThreads * kRefsPerThread);
    EXPECT_FALSE(ptr.has_outstanding_references());
    ptr.mark_and_delete_if_ready();
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(RefOwnerTest, DeleteIfDeleteableCalledMultipleTimes)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    ptr.mark_for_deletion();

    bool first  = ptr.delete_if_deleteable();
    bool second = ptr.delete_if_deleteable();

    EXPECT_TRUE(first);
    EXPECT_FALSE(second);  // Already deleted
    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

TEST_F(RefOwnerTest, RefReleasedAfterMarkButBeforeDelete)
{
    ref_owner<TestObject> ptr(new TestObject(42));
    auto ref = ptr.try_make_ref();

    ptr.mark_for_deletion();
    EXPECT_FALSE(ptr.delete_if_deleteable());  // Still has ref

    ref.reset();  // Release ref

    EXPECT_TRUE(ptr.delete_if_deleteable());  // Now deleteable
    EXPECT_EQ(TestObject::destruction_count.load(), 1);
}

// =============================================================================
// Reference Move Cast Tests
// =============================================================================

// Polymorphic base for dynamic_cast tests
struct PolyBase
{
    int base_value;
    explicit PolyBase(int v = 0) : base_value(v) {}
    virtual ~PolyBase() = default;
    virtual int get_value() const { return base_value; }
};

struct PolyDerived : PolyBase
{
    int derived_value;
    explicit PolyDerived(int b = 0, int d = 0) : PolyBase(b), derived_value(d) {}
    int get_value() const override { return derived_value; }
};

struct PolyOther : PolyBase
{
    int other_value;
    explicit PolyOther(int b = 0, int o = 0) : PolyBase(b), other_value(o) {}
    int get_value() const override { return other_value; }
};

TEST_F(RefOwnerTest, StaticReferenceMove_DerivedToBase)
{
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));
    auto derived_ref = ptr.try_make_ref();
    ASSERT_TRUE(derived_ref.has_value());

    // Move the reference and cast to base type
    unique_reference<PolyBase, PolyDerived> base_ref =
        zoox::static_reference_move<PolyBase>(std::move(*derived_ref));

    // Verify the cast reference works
    EXPECT_EQ(base_ref.get().base_value, 10);
    EXPECT_EQ(base_ref.get().get_value(), 20);  // Virtual call to derived

    // Ref count should still be 1
    EXPECT_EQ(ptr.ref_count(), 1);

    ptr.mark_and_delete_if_ready();  // Will fail - ref still exists
    EXPECT_FALSE(ptr.is_deleted());
}

TEST_F(RefOwnerTest, StaticReferenceMove_SourceMovedFrom)
{
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));
    auto derived_ref = ptr.make_ref();

    EXPECT_EQ(ptr.ref_count(), 1);

    // Move to base type
    auto base_ref = zoox::static_reference_move<PolyBase>(std::move(derived_ref));

    // Ref count unchanged (ownership transferred, not new ref)
    EXPECT_EQ(ptr.ref_count(), 1);

    // base_ref is valid
    EXPECT_EQ(base_ref.get().base_value, 10);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, DynamicReferenceMove_Success)
{
    // Store a Derived as Base, then dynamic cast back to Derived
    ref_owner<PolyBase> ptr(new PolyDerived(10, 20));
    auto base_ref = ptr.make_ref();

    EXPECT_EQ(ptr.ref_count(), 1);

    // Dynamic cast to derived - should succeed
    auto maybe_derived = zoox::dynamic_reference_move<PolyDerived>(std::move(base_ref));

    ASSERT_TRUE(maybe_derived.has_value());
    EXPECT_EQ(maybe_derived->get().derived_value, 20);

    // Ref count unchanged
    EXPECT_EQ(ptr.ref_count(), 1);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, DynamicReferenceMove_Failure_ReturnsNullopt)
{
    // Store a PolyOther as Base, then try to cast to PolyDerived
    ref_owner<PolyBase> ptr(new PolyOther(10, 30));
    auto base_ref = ptr.make_ref();

    // Dynamic cast to wrong derived type - should fail
    auto maybe_derived = zoox::dynamic_reference_move<PolyDerived>(std::move(base_ref));

    EXPECT_FALSE(maybe_derived.has_value());

    // Original ref should still work (not moved)
    // Note: After failed dynamic_reference_move, source ref is still valid
    EXPECT_EQ(ptr.ref_count(), 1);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, ConvertingMoveConstructor_Upcast)
{
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));
    unique_reference<PolyDerived> derived_ref = ptr.make_ref();

    // Use converting move constructor directly
    unique_reference<PolyBase, PolyDerived> base_ref(std::move(derived_ref));

    EXPECT_EQ(base_ref.get().base_value, 10);
    EXPECT_EQ(base_ref.get().get_value(), 20);
    EXPECT_EQ(ptr.ref_count(), 1);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, TwoParameterTemplate_ExplicitTypes)
{
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));

    // Explicit two-parameter form
    auto ref = ptr.make_ref();

    // Move to explicit two-parameter form with different ReferenceType
    unique_reference<PolyBase, PolyDerived> base_ref(std::move(ref));

    // Can access as Base
    PolyBase& base = base_ref.get();
    EXPECT_EQ(base.base_value, 10);

    // Virtual dispatch still works
    EXPECT_EQ(base.get_value(), 20);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, ReferenceMove_ChainedCasts)
{
    // Test chaining: Derived -> Base -> Interface (if we had one)
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));
    auto derived_ref = ptr.make_ref();

    // First cast: Derived -> Base
    auto base_ref = zoox::static_reference_move<PolyBase>(std::move(derived_ref));
    EXPECT_EQ(base_ref.get().base_value, 10);

    // Ref count still 1
    EXPECT_EQ(ptr.ref_count(), 1);

    ptr.mark_for_deletion();
}

TEST_F(RefOwnerTest, ReferenceMove_RefCountCorrectOnDestruction)
{
    ref_owner<PolyDerived> ptr(new PolyDerived(10, 20));

    {
        auto derived_ref = ptr.make_ref();
        EXPECT_EQ(ptr.ref_count(), 1);

        {
            auto base_ref = zoox::static_reference_move<PolyBase>(std::move(derived_ref));
            EXPECT_EQ(ptr.ref_count(), 1);
        }  // base_ref destroyed here

        // Ref count should be 0 now
        EXPECT_EQ(ptr.ref_count(), 0);
    }

    EXPECT_EQ(ptr.ref_count(), 0);
    ptr.mark_and_delete_if_ready();
    EXPECT_TRUE(ptr.is_deleted());
}

// =============================================================================
// Custom Deleter Tests
// =============================================================================

// Functor deleter that tracks deletion count
struct CountingDeleter
{
    static std::atomic<int> delete_count;

    void operator()(TestObject* p) const
    {
        delete_count.fetch_add(1);
        delete p;
    }
};

std::atomic<int> CountingDeleter::delete_count{0};

TEST_F(RefOwnerTest, CustomDeleter_FunctorDeleterIsCalled)
{
    CountingDeleter::delete_count.store(0);

    {
        ref_owner<TestObject, std::optional, CountingDeleter> ptr(new TestObject(42));
        EXPECT_EQ(ptr->value, 42);
        ptr.mark_and_delete_if_ready();
    }

    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_ConstructWithDeleterInstance)
{
    CountingDeleter::delete_count.store(0);

    CountingDeleter deleter;
    {
        ref_owner<TestObject, std::optional, CountingDeleter> ptr(new TestObject(99), deleter);
        EXPECT_EQ(ptr->value, 99);
        ptr.mark_and_delete_if_ready();
    }

    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_LambdaDeleter)
{
    std::atomic<int> lambda_delete_count{0};

    auto lambda_deleter = [&lambda_delete_count](TestObject* p) {
        lambda_delete_count.fetch_add(1);
        delete p;
    };

    {
        ref_owner<TestObject, std::optional, decltype(lambda_deleter)> ptr(new TestObject(77), lambda_deleter);
        EXPECT_EQ(ptr->value, 77);
        ptr.mark_and_delete_if_ready();
    }

    EXPECT_EQ(lambda_delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_DeleterNotCalledUntilDeletion)
{
    CountingDeleter::delete_count.store(0);

    ref_owner<TestObject, std::optional, CountingDeleter> ptr(new TestObject(55));

    // Create and destroy a reference - deleter should NOT be called
    {
        auto ref = ptr.make_ref();
        EXPECT_EQ(ref.get().value, 55);
    }

    EXPECT_EQ(CountingDeleter::delete_count.load(), 0);

    // Now delete - deleter SHOULD be called
    ptr.mark_and_delete_if_ready();
    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_WithUniquePtr)
{
    CountingDeleter::delete_count.store(0);

    auto uptr = std::unique_ptr<TestObject, CountingDeleter>(new TestObject(88));
    {
        ref_owner<TestObject, std::optional, CountingDeleter> ptr(std::move(uptr));
        EXPECT_EQ(ptr->value, 88);
        ptr.mark_and_delete_if_ready();
    }

    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_WaitableRefOwnerWithDeleter)
{
    CountingDeleter::delete_count.store(0);

    {
        waitable_ref_owner<TestObject, std::optional, CountingDeleter> ptr(new TestObject(33));
        EXPECT_EQ(ptr->value, 33);
        ptr.mark_and_wait_for_deletion();
    }

    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

TEST_F(RefOwnerTest, CustomDeleter_RefsWorkWithCustomDeleter)
{
    CountingDeleter::delete_count.store(0);

    ref_owner<TestObject, std::optional, CountingDeleter> ptr(new TestObject(44));

    {
        auto ref1 = ptr.make_ref();
        auto ref2 = ptr.try_make_ref();
        ASSERT_TRUE(ref2.has_value());

        EXPECT_EQ(ref1.get().value, 44);
        EXPECT_EQ(ref2->get().value, 44);
        EXPECT_EQ(ptr.ref_count(), 2);
    }

    EXPECT_EQ(ptr.ref_count(), 0);
    ptr.mark_and_delete_if_ready();
    EXPECT_EQ(CountingDeleter::delete_count.load(), 1);
}

// Stateful deleter test
struct StatefulDeleter
{
    int* counter;

    explicit StatefulDeleter(int* c) : counter(c) {}

    void operator()(TestObject* p) const
    {
        if (counter)
        {
            ++(*counter);
        }
        delete p;
    }
};

TEST_F(RefOwnerTest, CustomDeleter_StatefulDeleter)
{
    int counter = 0;
    StatefulDeleter deleter(&counter);

    {
        ref_owner<TestObject, std::optional, StatefulDeleter> ptr(new TestObject(66), deleter);
        EXPECT_EQ(ptr->value, 66);
        ptr.mark_and_delete_if_ready();
    }

    EXPECT_EQ(counter, 1);
}

}  // namespace
}  // namespace zoox
