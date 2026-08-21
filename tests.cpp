///Mostly AI generated, for the sake of learning
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include "vector.h"

// ---------------------------------------------------------------------------
// Instrumented element types
// ---------------------------------------------------------------------------

// Counts how many objects are alive, and how many times each special member
// ran. `live` going negative means a destructor ran on something that was
// never constructed. `live` staying positive after a scope means a leak.
struct Tracked {
    static int live;
    static int default_ctors;
    static int copy_ctors;
    static int assignments;
    static int dtors;

    int value;

    Tracked() : value(0) { ++live; ++default_ctors; }
    explicit Tracked(int v) : value(v) { ++live; ++default_ctors; }
    Tracked(const Tracked& o) : value(o.value) { ++live; ++copy_ctors; }
    Tracked& operator=(const Tracked& o) { value = o.value; ++assignments; return *this; }
    ~Tracked() { --live; ++dtors; }

    static void reset() {
        live = default_ctors = copy_ctors = assignments = dtors = 0;
    }
};

int Tracked::live = 0;
int Tracked::default_ctors = 0;
int Tracked::copy_ctors = 0;
int Tracked::assignments = 0;
int Tracked::dtors = 0;

// Throws from its copy constructor once `budget` copies have been made.
struct ThrowOnCopy {
    static int budget;  // copies allowed before the next one throws
    static int live;

    int value;

    explicit ThrowOnCopy(int v = 0) : value(v) { ++live; }

    ThrowOnCopy(const ThrowOnCopy& o) : value(o.value) {
        if (budget-- <= 0) throw std::runtime_error("copy ctor failed");
        ++live;
    }

    ~ThrowOnCopy() { --live; }

    static void reset(int b) { budget = b; live = 0; }
};

int ThrowOnCopy::budget = 0;
int ThrowOnCopy::live = 0;

// Throws from the DEFAULT constructor after `budget` successes. Needed to
// reach the count ctor, which ThrowOnCopy never exercises.
struct ThrowOnDefault {
    static int budget;
    static int live;

    int value;

    ThrowOnDefault() : value(0) {
        if (budget-- <= 0) throw std::runtime_error("default ctor failed");
        ++live;
    }
    ThrowOnDefault(const ThrowOnDefault& o) : value(o.value) { ++live; }
    ~ThrowOnDefault() { --live; }

    static void reset(int b) { budget = b; live = 0; }
};

int ThrowOnDefault::budget = 0;
int ThrowOnDefault::live = 0;

// No default constructor: only the fill and copy ctors may be instantiated
// for this type. Catches an accidental T() in the fill ctor at compile time.
struct NoDefault {
    int value;
    explicit NoDefault(int v) : value(v) {}
};

// Owns heap memory. A destructor call on raw bytes corrupts the heap; a
// missing destructor call leaks. Both show up under ASan.
struct Owning {
    std::string* p;
    explicit Owning(const char* s = "x") : p(new std::string(s)) {}
    Owning(const Owning& o) : p(new std::string(*o.p)) {}
    ~Owning() { delete p; }
};

// Over-aligned: malloc only guarantees alignof(std::max_align_t).
struct alignas(64) OverAligned {
    double payload[8];
    OverAligned() : payload{} {}
};

// ---------------------------------------------------------------------------
// Default constructor
// ---------------------------------------------------------------------------

TEST(DefaultCtor, IsEmpty) {
    vector<int> v;
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
}

TEST(DefaultCtor, DestructorOnEmptyIsSafe) {
    // free(nullptr) is a no-op; the destroy loop runs zero times.
    { vector<int> v; }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Count constructor
// ---------------------------------------------------------------------------

TEST(CountCtor, SetsSize) {
    vector<int> v(5);
    EXPECT_EQ(v.size(), 5u);
    EXPECT_EQ(v.capacity(), 5u);
}

TEST(CountCtor, ZeroCountIsEmpty) {
    vector<int> v(0);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u) << "early return must leave capacity_ at 0";
}

TEST(CountCtor, SizeOneBoundary) {
    vector<int> v(1);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0);
}

TEST(CountCtor, ValueInitializesScalars) {
    // The standard requires value-initialization: T() not T.
    // If you ever switch the loop to `new (p) T;` this test starts failing
    // (or reading garbage) for int.
    vector<int> v(8);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) {
        EXPECT_EQ(v[i], 0) << "element " << i << " was not value-initialized";
    }
}

TEST(CountCtor, ValueInitializesTrivialAggregates) {
    // Same distinction, for a struct with no user-provided constructor.
    struct Point { int x; int y; };
    vector<Point> v(3);
    for (vector<Point>::size_type i = 0; i < v.size(); ++i) {
        EXPECT_EQ(v[i].x, 0);
        EXPECT_EQ(v[i].y, 0);
    }
}

TEST(CountCtor, RunsOneDefaultCtorPerElement) {
    Tracked::reset();
    {
        vector<Tracked> v(4);
        EXPECT_EQ(Tracked::default_ctors, 4);
        EXPECT_EQ(Tracked::live, 4);
        EXPECT_EQ(Tracked::assignments, 0);  // constructed, never assigned
    }
    EXPECT_EQ(Tracked::live, 0) << "destructor did not destroy every element";
}

// ---------------------------------------------------------------------------
// Fill constructor
// ---------------------------------------------------------------------------

TEST(FillCtor, SetsSizeAndValues) {
    vector<int> v(3, 42);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 42);
    EXPECT_EQ(v[1], 42);
    EXPECT_EQ(v[2], 42);
}

TEST(FillCtor, ZeroCountIsEmpty) {
    vector<int> v(0, 42);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
}

TEST(FillCtor, ConstructsRatherThanAssigns) {
    // This is the placement-new-vs-assignment distinction.
    // `data_[i] = value` would show up here as assignments == 5.
    Tracked::reset();
    {
        Tracked proto(7);
        vector<Tracked> v(5, proto);
        EXPECT_EQ(Tracked::copy_ctors, 5);
        EXPECT_EQ(Tracked::assignments, 0);
        EXPECT_EQ(Tracked::live, 6);  // 5 elements + proto
        EXPECT_EQ(v[0].value, 7);
        EXPECT_EQ(v[4].value, 7);
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(FillCtor, EachElementIsIndependent) {
    // Distinct objects, not one object referenced count times.
    vector<Tracked> v(3, Tracked(1));
    v[0].value = 99;
    EXPECT_EQ(v[1].value, 1) << "elements share storage";
    EXPECT_EQ(v[2].value, 1);
}

TEST(FillCtor, WorksForTypeWithNoDefaultCtor) {
    // Only compiles if the fill ctor never needs T().
    vector<NoDefault> v(3, NoDefault(5));
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[1].value, 5);
}

// ---------------------------------------------------------------------------
// Copy constructor
// ---------------------------------------------------------------------------

TEST(CopyCtor, PreservesSizeAndValues) {
    vector<int> a(4, 9);
    vector<int> b(a);
    ASSERT_EQ(b.size(), a.size());
    for (vector<int>::size_type i = 0; i < b.size(); ++i) {
        EXPECT_EQ(b[i], 9);
    }
}

TEST(CopyCtor, IsADeepCopy) {
    // Requires operator[] to return a reference.
    vector<int> a(3, 1);
    vector<int> b(a);
    b[0] = 99;
    EXPECT_EQ(a[0], 1) << "writing to the copy modified the original";
    EXPECT_EQ(b[0], 99);
}

TEST(CopyCtor, BuffersAreDistinct) {
    vector<int> a(3, 1);
    vector<int> b(a);
    EXPECT_NE(&a[0], &b[0]) << "the copy points at the source's buffer";
}

TEST(CopyCtor, CopyConstructsEachElement) {
    Tracked::reset();
    {
        vector<Tracked> a(3);
        EXPECT_EQ(Tracked::live, 3);
        vector<Tracked> b(a);
        EXPECT_EQ(Tracked::copy_ctors, 3);
        EXPECT_EQ(Tracked::live, 6) << "copy shares storage instead of owning its own";
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(CopyCtor, CopyingAnEmptyVectorIsSafe) {
    vector<int> a;
    vector<int> b(a);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.capacity(), 0u);
}

TEST(CopyCtor, AcceptsAConstSource) {
    // Fails to compile if size() or operator[] is missing its const overload.
    const vector<int> a(2, 5);
    vector<int> b(a);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[1], 5);
}

TEST(CopyCtor, CapacityMatchesWhatWasAllocated) {
    // The ctor allocates other.size() elements, so capacity_ must be
    // other.size() too. Copying other.capacity_ instead would claim room the
    // buffer doesn't have, and the first push_back would run off the end.
    vector<int> a(4, 1);
    vector<int> b(a);
    EXPECT_EQ(b.capacity(), b.size());
}

TEST(CopyCtor, SurvivesTheSourceGoingAway) {
    vector<int>* a = new vector<int>(3, 8);
    vector<int> b(*a);
    delete a;
    EXPECT_EQ(b[0], 8) << "the copy was pointing into the source's buffer";
    EXPECT_EQ(b[2], 8);
}

TEST(CopyCtor, NestedVectors) {
    // Exercises the copy ctor recursively: T is itself a vector.
    vector<vector<int>> a(2, vector<int>(3, 7));
    vector<vector<int>> b(a);
    ASSERT_EQ(b.size(), 2u);
    ASSERT_EQ(b[0].size(), 3u);
    EXPECT_EQ(b[1][2], 7);
    b[0][0] = 100;
    EXPECT_EQ(a[0][0], 7) << "inner buffers are shared between the copies";
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

TEST(Dtor, DestroysExactlyOncePerElement) {
    Tracked::reset();
    {
        vector<Tracked> v(10);
    }
    EXPECT_EQ(Tracked::dtors, 10);
    EXPECT_EQ(Tracked::live, 0);
}

TEST(Dtor, ReleasesElementResources) {
    // Run under ASan: a missing ~T() leaks 16 std::strings.
    { vector<Owning> v(16, Owning("hello")); }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------

// A constructor that throws never completes, so ~vector() is NOT called for it.
// Whatever the constructor already built is therefore its own responsibility:
// destroy the elements placement new actually reached, free the buffer, rethrow.
//
// Destroying too many is undefined behaviour (~T() on raw bytes); destroying too
// few leaks. `live` catches both -- below the expected value means
// over-destruction, above means a leak.

TEST(ExceptionSafety, FillCtorDestroysOnlyConstructedElements) {
    ThrowOnCopy::reset(/*budget=*/3);
    ThrowOnCopy proto(1);
    ASSERT_EQ(ThrowOnCopy::live, 1);

    EXPECT_THROW({ vector<ThrowOnCopy> v(10, proto); }, std::runtime_error);

    // The 3 successful copies are destroyed, the 7 slots placement new never
    // reached are left alone, the buffer is freed. Only proto survives.
    EXPECT_EQ(ThrowOnCopy::live, 1);
}

TEST(ExceptionSafety, FillCtorThrowOnTheVeryFirstElement) {
    ThrowOnCopy::reset(/*budget=*/0);
    ThrowOnCopy proto(1);

    EXPECT_THROW({ vector<ThrowOnCopy> v(4, proto); }, std::runtime_error);

    // size_ is still 0, so the cleanup loop must not run at all.
    EXPECT_EQ(ThrowOnCopy::live, 1);
}

TEST(ExceptionSafety, FillCtorThrowOnTheLastElement) {
    ThrowOnCopy::reset(/*budget=*/3);
    ThrowOnCopy proto(1);

    EXPECT_THROW({ vector<ThrowOnCopy> v(4, proto); }, std::runtime_error);
    EXPECT_EQ(ThrowOnCopy::live, 1);
}

TEST(ExceptionSafety, FillCtorPropagatesTheOriginalException) {
    // partial_destruction() ends in a bare `throw;`. That is a rethrow of the
    // exception currently being handled -- legal only because the function is
    // called from inside a catch block. Outside one it would call terminate.
    ThrowOnCopy::reset(/*budget=*/1);
    ThrowOnCopy proto(1);
    try {
        vector<ThrowOnCopy> v(5, proto);
        FAIL() << "expected the copy ctor to throw";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "copy ctor failed");
    } catch (...) {
        FAIL() << "the wrong exception type escaped";
    }
}

TEST(ExceptionSafety, CountCtorDestroysOnlyConstructedElements) {
    // T() can throw just as easily as T(value); the count ctor needs the same
    // cleanup as the fill ctor. A missing (or dead) try block shows up here as
    // `live` stuck at 3 -- the already-built elements were never destroyed.
    ThrowOnDefault::reset(/*budget=*/3);

    EXPECT_THROW({ vector<ThrowOnDefault> v(10); }, std::runtime_error);

    EXPECT_EQ(ThrowOnDefault::live, 0)
        << "count ctor leaked the elements it had already constructed";
}

TEST(ExceptionSafety, CountCtorThrowOnTheVeryFirstElement) {
    ThrowOnDefault::reset(/*budget=*/0);
    EXPECT_THROW({ vector<ThrowOnDefault> v(4); }, std::runtime_error);
    EXPECT_EQ(ThrowOnDefault::live, 0);
}

TEST(ExceptionSafety, CopyCtorCleansUpPartialWork) {
    ThrowOnCopy::reset(/*budget=*/100);
    {
        vector<ThrowOnCopy> source(6, ThrowOnCopy(2));
        ASSERT_EQ(ThrowOnCopy::live, 6);  // the temporary proto is already gone

        ThrowOnCopy::budget = 2;  // allow only 2 of the 6 copies
        EXPECT_THROW({ vector<ThrowOnCopy> copy(source); },
                     std::runtime_error);

        EXPECT_EQ(ThrowOnCopy::live, 6) << "the failed copy damaged the source";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}

TEST(ExceptionSafety, FailedConstructionLeaksNoBuffer) {
    // Purely an ASan/valgrind check: the malloc'd block must be freed on the
    // throwing path. Looping makes any leak big enough to be obvious.
    for (int i = 0; i < 100; ++i) {
        ThrowOnCopy::reset(/*budget=*/5);
        ThrowOnCopy proto(1);
        EXPECT_THROW({ vector<ThrowOnCopy> v(1000, proto); }, std::runtime_error);
    }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Allocation edge cases
// ---------------------------------------------------------------------------

TEST(Alloc, HugeRequestThrowsBadAlloc) {
    // malloc returns nullptr rather than throwing, so allocate_raw has to check
    // and `throw std::bad_alloc();` itself. A bare `throw;` here would call
    // terminate: there is no exception in flight to rethrow.
    const auto huge = std::numeric_limits<vector<int>::size_type>::max() / 2;
    EXPECT_THROW({ vector<int> v(huge); }, std::bad_alloc);
}

TEST(Alloc, DISABLED_SizeOverflowIsDetected) {
    // count * sizeof(T) wraps around, so a small block gets allocated and the
    // constructor then writes far past it. Needs an explicit check before the
    // multiplication:
    //   if (count > std::numeric_limits<size_type>::max() / sizeof(T))
    //       throw std::length_error("vector too long");
    using ST = vector<std::int64_t>::size_type;
    const ST count = std::numeric_limits<ST>::max() / 4 + 1;  // *8 overflows
    EXPECT_THROW({ vector<std::int64_t> v(count); }, std::length_error);
}

TEST(Alloc, DISABLED_RespectsOverAlignedTypes) {
    // malloc only guarantees alignof(std::max_align_t), typically 16. A
    // 64-byte-aligned element needs aligned_alloc, or the aligned overload of
    // ::operator new.
    vector<OverAligned> v(4);
    for (vector<OverAligned>::size_type i = 0; i < v.size(); ++i) {
        auto addr = reinterpret_cast<std::uintptr_t>(&v[i]);
        EXPECT_EQ(addr % alignof(OverAligned), 0u) << "element " << i << " misaligned";
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

TEST(Accessors, SubscriptReturnsAReference) {
    vector<int> v(3, 1);
    v[1] = 77;
    EXPECT_EQ(v[1], 77) << "operator[] returns by value, so the write was discarded";
}

TEST(Accessors, ElementsAreContiguous) {
    vector<int> v(4, 0);
    EXPECT_EQ(&v[0] + 1, &v[1]);
    EXPECT_EQ(&v[0] + 3, &v[3]);
}

TEST(Accessors, ConstSubscriptReturnsAConstReference) {
    const vector<int> v(3, 5);
    const int& r = v[0];
    EXPECT_EQ(r, 5);
    EXPECT_EQ(&r, &v[0]) << "const operator[] handed back a temporary copy";
}

TEST(Accessors, SizeAndCapacityAreCallableOnConst) {
    const vector<int> v(2, 1);
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v.capacity(), 2u);
}


// ---------------------------------------------------------------------------
// Move constructor
// ---------------------------------------------------------------------------
 
TEST(MoveCtor, IsMarkedNoexcept) {
    // Without this, vector's own reallocation logic falls back to copying
    // (std::move_if_noexcept), and every growth silently loses the speedup.
    static_assert(std::is_nothrow_move_constructible<vector<int>>::value,
                  "the move constructor must be noexcept");
    SUCCEED();
}
 
TEST(MoveCtor, TransfersSizeAndValues) {
    vector<int> a(4, 9);
    vector<int> b(std::move(a));
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b.capacity(), 4u);
    EXPECT_EQ(b[0], 9);
    EXPECT_EQ(b[3], 9);
}
 
TEST(MoveCtor, StealsTheBufferRatherThanCopyingIt) {
    // The defining property: the new vector points at the SAME memory.
    vector<int> a(3, 1);
    const int* old_buffer = &a[0];
 
    vector<int> b(std::move(a));
    EXPECT_EQ(&b[0], old_buffer) << "the buffer was reallocated, not transferred";
}
 
TEST(MoveCtor, LeavesTheSourceEmpty) {
    // "Valid but unspecified" -- empty is the conventional choice, and it is
    // what stops the source's destructor from freeing the stolen buffer.
    vector<int> a(5, 1);
    vector<int> b(std::move(a));
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.capacity(), 0u);
}
 
TEST(MoveCtor, ConstructsNoElements) {
    // No allocation and no element construction: only three scalars are read
    // and three are written.
    Tracked::reset();
    {
        vector<Tracked> a(6);
        ASSERT_EQ(Tracked::default_ctors, 6);
 
        vector<Tracked> b(std::move(a));
        EXPECT_EQ(Tracked::copy_ctors, 0) << "the move ctor copied the elements";
        EXPECT_EQ(Tracked::default_ctors, 6) << "extra elements were constructed";
        EXPECT_EQ(Tracked::dtors, 0) << "the move ctor destroyed something";
        EXPECT_EQ(Tracked::live, 6);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveCtor, ElementsAreDestroyedExactlyOnce) {
    // The moved-from vector must NOT destroy the elements it gave away.
    // A double destruction shows up as live == -6.
    Tracked::reset();
    {
        vector<Tracked> a(6);
        vector<Tracked> b(std::move(a));
    }
    EXPECT_EQ(Tracked::dtors, 6) << "elements were destroyed twice, or not at all";
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveCtor, MovedFromSourceIsSafeToDestroy) {
    // Run under ASan: if data_ was not nulled, both destructors free the same
    // block and this is a double free.
    {
        vector<Owning> a(8, Owning("payload"));
        vector<Owning> b(std::move(a));
    }
    SUCCEED();
}
 
TEST(MoveCtor, SourceOutlivingTheDestinationIsSafe) {
    // Reverse lifetime order: the destination dies first and frees the buffer.
    // The source must not touch it afterwards.
    vector<Owning> a(4, Owning("payload"));
    {
        vector<Owning> b(std::move(a));
    }
    EXPECT_EQ(a.size(), 0u);
}
 
TEST(MoveCtor, MovingAnEmptyVectorIsSafe) {
    vector<int> a;
    vector<int> b(std::move(a));
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.capacity(), 0u);
    EXPECT_EQ(a.size(), 0u);
}
 
TEST(MoveCtor, MovedFromVectorCanBeCopiedFrom) {
    // The source must be a usable object, not merely destructible.
    vector<int> a(3, 1);
    vector<int> b(std::move(a));
    vector<int> c(a);  // copy of the empty moved-from vector
    EXPECT_EQ(c.size(), 0u);
}
 
TEST(MoveCtor, ChainedMovesKeepOneOwner) {
    vector<int> a(3, 4);
    const int* buffer = &a[0];
 
    vector<int> b(std::move(a));
    vector<int> c(std::move(b));
 
    EXPECT_EQ(&c[0], buffer) << "the buffer was copied somewhere along the chain";
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(c.size(), 3u);
}
 
TEST(MoveCtor, PreferredOverCopyForRvalues) {
    // Overload resolution check: an rvalue must select vector(vector&&).
    // If the move ctor is missing, this silently binds to the copy ctor and
    // copy_ctors becomes 4.
    Tracked::reset();
    {
        vector<Tracked> a(4);
        vector<Tracked> b(static_cast<vector<Tracked>&&>(a));
        EXPECT_EQ(Tracked::copy_ctors, 0) << "an rvalue selected the copy ctor";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveCtor, LvaluesStillSelectTheCopyCtor) {
    // The mirror image: adding a move ctor must not divert plain copies.
    Tracked::reset();
    {
        vector<Tracked> a(4);
        vector<Tracked> b(a);
        EXPECT_EQ(Tracked::copy_ctors, 4) << "an lvalue was moved from";
        EXPECT_EQ(a.size(), 4u) << "the source was gutted by a copy";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveCtor, WorksForElementsThatCannotBeCopied) {
    // ThrowOnCopy throws once its budget runs out. With budget 0 any element
    // copy fails, so this only passes if the move genuinely copies nothing.
    ThrowOnCopy::reset(/*budget=*/6);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> a(6, proto);
        ASSERT_EQ(ThrowOnCopy::budget, 0);
 
        EXPECT_NO_THROW({ vector<ThrowOnCopy> b(std::move(a)); });
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(MoveCtor, NestedVectors) {
    vector<vector<int>> a(2, vector<int>(3, 7));
    const int* inner = &a[0][0];
 
    vector<vector<int>> b(std::move(a));
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[1][2], 7);
    EXPECT_EQ(&b[0][0], inner) << "the inner buffers were reallocated";
    EXPECT_EQ(a.size(), 0u);
}
 
TEST(MoveCtor, LargeVectorCostsNothing) {
    // Not a timing test -- just proof that no per-element work happens for a
    // vector far too large to copy cheaply.
    Tracked::reset();
    {
        vector<Tracked> a(10000);
        Tracked::copy_ctors = 0;
        vector<Tracked> b(std::move(a));
        EXPECT_EQ(Tracked::copy_ctors, 0);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 