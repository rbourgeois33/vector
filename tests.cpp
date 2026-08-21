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
    Owning& operator=(const Owning& o) {
        *p = *o.p;              // reuse the existing string, no realloc
        return *this;
    }
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
    EXPECT_THROW({ vector<int> v(huge); }, std::length_error);
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


// Copy assignment tests -- paste into vector_test.cpp after the move ctor
// section. Reuses Tracked, ThrowOnCopy, NoDefault and Owning.
//
// The three branches under test:
//   A. capacity_ < other.size_   -> reallocate (strong guarantee)
//   B. size_ < other.size_       -> assign prefix, construct the rest (basic)
//   C. size_ >= other.size_      -> assign prefix, destroy the tail (basic)

#include <utility>

// Throws from operator= after `budget` successful assignments. The copy ctor
// never throws, so only the reuse branches are affected.
struct ThrowOnAssign {
    static int budget;
    static int live;

    int value;

    explicit ThrowOnAssign(int v = 0) : value(v) { ++live; }
    ThrowOnAssign(const ThrowOnAssign& o) : value(o.value) { ++live; }
    ThrowOnAssign& operator=(const ThrowOnAssign& o) {
        if (budget-- <= 0) throw std::runtime_error("assignment failed");
        value = o.value;
        return *this;
    }
    ~ThrowOnAssign() { --live; }

    static void reset(int b) { budget = b; live = 0; }
};

int ThrowOnAssign::budget = 0;
int ThrowOnAssign::live = 0;

// ---------------------------------------------------------------------------
// Return value and chaining
// ---------------------------------------------------------------------------

TEST(CopyAssign, ReturnsReferenceToThis) {
    vector<int> a(2, 1);
    vector<int> b(3, 7);
    vector<int>& r = (a = b);
    EXPECT_EQ(&r, &a) << "operator= returned a copy instead of *this";
}

TEST(CopyAssign, SupportsChaining) {
    vector<int> a(1, 0), b(1, 0), c(3, 5);
    a = b = c;
    ASSERT_EQ(a.size(), 3u);
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(a[2], 5);
    EXPECT_EQ(b[2], 5);
}

// ---------------------------------------------------------------------------
// Self-assignment
// ---------------------------------------------------------------------------

TEST(CopyAssign, SelfAssignmentPreservesContents) {
    // Without the this == &other guard, the reuse branches read and write the
    // same buffer and the tail gets destroyed after being "copied".
    vector<int> a(4, 3);
    a = a;
    ASSERT_EQ(a.size(), 4u);
    for (vector<int>::size_type i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], 3);
}

TEST(CopyAssign, SelfAssignmentDoesNoWork) {
    Tracked::reset();
    {
        vector<Tracked> a(4);
        Tracked::assignments = 0;
        Tracked::dtors = 0;

        a = a;

        EXPECT_EQ(Tracked::assignments, 0) << "self-assignment did element work";
        EXPECT_EQ(Tracked::dtors, 0) << "self-assignment destroyed elements";
        EXPECT_EQ(Tracked::live, 4);
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(CopyAssign, SelfAssignmentThroughAReferenceIsSafe) {
    // The realistic way self-assignment happens.
    vector<Owning> a(3, Owning("payload"));
    const vector<Owning>& alias = a;
    a = alias;
    EXPECT_EQ(a.size(), 3u);
}

// ---------------------------------------------------------------------------
// Branch A: reallocation (capacity_ < other.size_)
// ---------------------------------------------------------------------------

TEST(CopyAssign, GrowingBeyondCapacityReallocates) {
    vector<int> a(2, 1);
    vector<int> b(10, 4);
    a = b;
    ASSERT_EQ(a.size(), 10u);
    EXPECT_GE(a.capacity(), 10u);
    EXPECT_EQ(a[0], 4);
    EXPECT_EQ(a[9], 4);
}

TEST(CopyAssign, GrowingFromEmpty) {
    vector<int> a;
    vector<int> b(3, 6);
    a = b;
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[1], 6);
}

TEST(CopyAssign, ReallocationDestroysTheOldElements) {
    Tracked::reset();
    {
        vector<Tracked> a(2);
        vector<Tracked> b(10);
        ASSERT_EQ(Tracked::live, 12);

        a = b;
        // a's 2 old elements destroyed, 10 new copies built: 10 + 10 = 20 live.
        EXPECT_EQ(Tracked::live, 20) << "old elements leaked or were double-destroyed";
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(CopyAssign, ReallocationUsesCopyConstructionNotAssignment) {
    // Nothing exists in the fresh buffer, so every element must be constructed.
    Tracked::reset();
    {
        vector<Tracked> a(2);
        vector<Tracked> b(10);
        Tracked::copy_ctors = 0;
        Tracked::assignments = 0;

        a = b;

        EXPECT_EQ(Tracked::copy_ctors, 10);
        EXPECT_EQ(Tracked::assignments, 0) << "assigned into raw memory";
    }
    EXPECT_EQ(Tracked::live, 0);
}

// ---------------------------------------------------------------------------
// Branch B: growing within existing capacity
// ---------------------------------------------------------------------------

TEST(CopyAssign, GrowingWithinCapacityReusesTheBuffer) {
    // Build spare capacity by assigning a large vector then a smaller one.
    vector<int> a(10, 1);
    vector<int> small(2, 2);
    a = small;
    ASSERT_EQ(a.size(), 2u);
    const int* buffer = &a[0];

    vector<int> mid(7, 3);
    a = mid;  // 7 <= capacity, no reallocation expected

    ASSERT_EQ(a.size(), 7u);
    EXPECT_EQ(&a[0], buffer) << "reallocated despite sufficient capacity";
    EXPECT_EQ(a[0], 3);
    EXPECT_EQ(a[6], 3);
}

TEST(CopyAssign, GrowingWithinCapacityMixesAssignmentAndConstruction) {
    Tracked::reset();
    {
        vector<Tracked> a(10);
        vector<Tracked> two(2);
        a = two;                      // a now size 2, capacity 10
        ASSERT_EQ(a.size(), 2u);

        vector<Tracked> seven(7);
        Tracked::assignments = 0;
        Tracked::copy_ctors = 0;

        a = seven;

        EXPECT_EQ(Tracked::assignments, 2) << "the existing 2 elements should be assigned";
        EXPECT_EQ(Tracked::copy_ctors, 5) << "the 5 new slots should be constructed";
    }
    EXPECT_EQ(Tracked::live, 0);
}

// ---------------------------------------------------------------------------
// Branch C: shrinking
// ---------------------------------------------------------------------------

TEST(CopyAssign, ShrinkingKeepsTheBufferAndCapacity) {
    vector<int> a(10, 1);
    const int* buffer = &a[0];
    const auto cap = a.capacity();

    vector<int> b(3, 8);
    a = b;

    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(&a[0], buffer) << "shrinking should not reallocate";
    EXPECT_EQ(a.capacity(), cap) << "shrinking should not reduce capacity";
    EXPECT_EQ(a[2], 8);
}

TEST(CopyAssign, ShrinkingDestroysTheTail) {
    Tracked::reset();
    {
        vector<Tracked> a(10);
        vector<Tracked> b(3);
        ASSERT_EQ(Tracked::live, 13);

        a = b;
        // a: 3 assigned-over survivors + 7 destroyed. b: 3. Total 6.
        EXPECT_EQ(Tracked::live, 6) << "tail elements were leaked or double-destroyed";
        EXPECT_EQ(a.size(), 3u);
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(CopyAssign, ShrinkingReleasesTailResources) {
    // ASan: the 7 discarded strings must be freed, exactly once.
    vector<Owning> a(10, Owning("payload"));
    vector<Owning> b(3, Owning("payload"));
    a = b;
    EXPECT_EQ(a.size(), 3u);
}

TEST(CopyAssign, AssigningEmptyEmptiesTheTarget) {
    Tracked::reset();
    {
        vector<Tracked> a(5);
        vector<Tracked> empty;
        a = empty;
        EXPECT_EQ(a.size(), 0u);
        EXPECT_EQ(Tracked::live, 0) << "elements were not destroyed";
    }
    EXPECT_EQ(Tracked::live, 0);
}

TEST(CopyAssign, SameSizeAssignsEveryElement) {
    Tracked::reset();
    {
        vector<Tracked> a(4);
        vector<Tracked> b(4);
        Tracked::assignments = 0;
        Tracked::copy_ctors = 0;
        Tracked::dtors = 0;

        a = b;

        EXPECT_EQ(Tracked::assignments, 4);
        EXPECT_EQ(Tracked::copy_ctors, 0) << "constructed instead of assigned";
        EXPECT_EQ(Tracked::dtors, 0) << "destroyed instead of assigned";
    }
    EXPECT_EQ(Tracked::live, 0);
}

// ---------------------------------------------------------------------------
// Independence and source integrity
// ---------------------------------------------------------------------------

TEST(CopyAssign, ResultIsIndependentOfTheSource) {
    vector<int> a(2, 1);
    vector<int> b(5, 9);
    a = b;
    a[0] = 100;
    EXPECT_EQ(b[0], 9) << "assignment produced a shallow copy";
    EXPECT_NE(&a[0], &b[0]);
}

TEST(CopyAssign, SourceIsUnchanged) {
    vector<int> a(2, 1);
    vector<int> b(5, 9);
    a = b;
    ASSERT_EQ(b.size(), 5u);
    EXPECT_EQ(b[4], 9);
}

TEST(CopyAssign, TargetSurvivesTheSourceGoingAway) {
    vector<int> a(1, 0);
    {
        vector<int> b(4, 2);
        a = b;
    }
    ASSERT_EQ(a.size(), 4u);
    EXPECT_EQ(a[3], 2);
}

TEST(CopyAssign, RepeatedAssignmentDoesNotLeak) {
    // ASan: a missing free on the reallocating path shows up here.
    vector<Owning> a;
    for (int i = 0; i < 200; ++i) {
        vector<Owning> b(static_cast<vector<Owning>::size_type>(i % 20 + 1),
                         Owning("payload"));
        a = b;
    }
    SUCCEED();
}

TEST(CopyAssign, WorksForTypeWithNoDefaultCtor) {
    // Assignment must never need T(). Fails to compile if the reallocating
    // branch default-constructs before assigning.
    vector<NoDefault> a(2, NoDefault(1));
    vector<NoDefault> b(5, NoDefault(9));
    a = b;
    ASSERT_EQ(a.size(), 5u);
    EXPECT_EQ(a[4].value, 9);
}

TEST(CopyAssign, ElementsRemainContiguous) {
    vector<int> a(2, 1);
    vector<int> b(6, 3);
    a = b;
    EXPECT_EQ(&a[0] + 5, &a[5]);
}

// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------

TEST(CopyAssign, ReallocatingBranchGivesTheStrongGuarantee) {
    // The new buffer is built before the old one is touched, so a throw must
    // leave the target exactly as it was.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> a(3, proto);
        vector<ThrowOnCopy> b(20, proto);
        const ThrowOnCopy* buffer = &a[0];
        const int before = ThrowOnCopy::live;

        ThrowOnCopy::budget = 5;  // fails partway through the 20 copies
        EXPECT_THROW({ a = b; }, std::runtime_error);

        EXPECT_EQ(a.size(), 3u) << "the target was modified by a failed assignment";
        EXPECT_EQ(&a[0], buffer) << "the target's buffer was released";
        EXPECT_EQ(ThrowOnCopy::live, before) << "the partial buffer leaked";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}

TEST(CopyAssign, ReusingBranchGivesTheBasicGuarantee) {
    // T::operator= throwing leaves every element valid -- the invariant holds,
    // only the values are unspecified. The vector must NOT be demolished:
    // it is a live object whose destructor will run later.
    ThrowOnAssign::reset(/*budget=*/1000);
    {
        vector<ThrowOnAssign> a(6, ThrowOnAssign(1));
        vector<ThrowOnAssign> b(6, ThrowOnAssign(2));

        ThrowOnAssign::budget = 3;  // fails on the 4th of 6 assignments
        EXPECT_THROW({ a = b; }, std::runtime_error);

        EXPECT_EQ(a.size(), 6u) << "the vector was gutted by a recoverable failure";
        EXPECT_EQ(a.capacity(), 6u);
        EXPECT_NO_THROW({ volatile int x = a[0].value; (void)x; })
            << "elements are no longer readable";
    }
    EXPECT_EQ(ThrowOnAssign::live, 0) << "cleanup on the throwing path was wrong";
}

TEST(CopyAssign, FailedAssignmentLeavesADestructibleObject) {
    // The most important property: whatever state the throw leaves behind,
    // ~vector() must handle it. ASan catches a double free or a dangling free.
    ThrowOnAssign::reset(/*budget=*/1000);
    {
        vector<ThrowOnAssign> a(8, ThrowOnAssign(1));
        vector<ThrowOnAssign> b(8, ThrowOnAssign(2));
        ThrowOnAssign::budget = 2;
        EXPECT_THROW({ a = b; }, std::runtime_error);
    }
    EXPECT_EQ(ThrowOnAssign::live, 0);
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------
 
TEST(MoveAssign, IsMarkedNoexcept) {
    // std::swap derives its own noexcept from this one, which in turn governs
    // every algorithm that swaps.
    static_assert(std::is_nothrow_move_assignable<vector<int>>::value,
                  "move assignment must be noexcept");
    SUCCEED();
}
 
TEST(MoveAssign, ReturnsReferenceToThis) {
    vector<int> a(2, 1);
    vector<int> b(3, 7);
    vector<int>& r = (a = std::move(b));
    EXPECT_EQ(&r, &a);
}
 
TEST(MoveAssign, SupportsChaining) {
    vector<int> a(1, 0), b(1, 0), c(3, 5);
    a = b = std::move(c);
    ASSERT_EQ(a.size(), 3u);
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(a[2], 5);
}
 
// ---------------------------------------------------------------------------
// Overload resolution
// ---------------------------------------------------------------------------
 
TEST(MoveAssign, RvaluesSelectMoveNotCopy) {
    // An rvalue binds happily to const vector&, so without the && overload
    // this silently deep-copies and copy_ctors becomes 5.
    Tracked::reset();
    {
        vector<Tracked> a(2);
        vector<Tracked> b(5);
        Tracked::copy_ctors = 0;
 
        a = std::move(b);
 
        EXPECT_EQ(Tracked::copy_ctors, 0) << "an rvalue selected copy assignment";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveAssign, LvaluesStillSelectCopy) {
    // The mirror image: adding the && overload must not divert plain copies.
    Tracked::reset();
    {
        vector<Tracked> a(2);
        vector<Tracked> b(5);
        Tracked::copy_ctors = 0;
 
        a = b;
 
        EXPECT_EQ(Tracked::copy_ctors, 5) << "an lvalue was moved from";
        EXPECT_EQ(b.size(), 5u) << "the source was gutted by a copy";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// The transfer itself
// ---------------------------------------------------------------------------
 
TEST(MoveAssign, StealsTheBuffer) {
    // The defining property: same memory, not a reallocation.
    vector<int> a(2, 1);
    vector<int> b(6, 9);
    const int* buffer = &b[0];
 
    a = std::move(b);
 
    ASSERT_EQ(a.size(), 6u);
    EXPECT_EQ(&a[0], buffer) << "the buffer was copied, not transferred";
    EXPECT_EQ(a[5], 9);
}
 
TEST(MoveAssign, TransfersCapacityToo) {
    vector<int> big(10, 1);
    vector<int> small(2, 2);
    big = small;                 // big keeps capacity 10, size 2
    const auto cap = big.capacity();
    ASSERT_GE(cap, 10u);
 
    vector<int> dest;
    dest = std::move(big);
    EXPECT_EQ(dest.capacity(), cap) << "capacity was not carried over";
    EXPECT_EQ(dest.size(), 2u);
}
 
TEST(MoveAssign, LeavesTheSourceEmpty) {
    vector<int> a(2, 1);
    vector<int> b(5, 3);
    a = std::move(b);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.capacity(), 0u);
}
 
TEST(MoveAssign, ConstructsNoElements) {
    Tracked::reset();
    {
        vector<Tracked> a(3);
        vector<Tracked> b(7);
        Tracked::copy_ctors = 0;
        Tracked::default_ctors = 0;
        Tracked::assignments = 0;
 
        a = std::move(b);
 
        EXPECT_EQ(Tracked::copy_ctors, 0);
        EXPECT_EQ(Tracked::default_ctors, 0);
        EXPECT_EQ(Tracked::assignments, 0) << "elements were assigned one by one";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveAssign, ReleasesTheTargetsOldContents) {
    // The job the move constructor does not have: dispose of what we own first.
    Tracked::reset();
    {
        vector<Tracked> a(3);
        vector<Tracked> b(7);
        ASSERT_EQ(Tracked::live, 10);
 
        a = std::move(b);
 
        EXPECT_EQ(Tracked::live, 7) << "the target's 3 old elements leaked";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveAssign, OldTargetResourcesAreFreed) {
    // ASan: the target's original buffer and its 10 strings must be released.
    vector<Owning> a(10, Owning("old"));
    vector<Owning> b(2, Owning("new"));
    a = std::move(b);
    EXPECT_EQ(a.size(), 2u);
}
 
TEST(MoveAssign, ElementsAreDestroyedExactlyOnce) {
    Tracked::reset();
    {
        vector<Tracked> a(3);
        vector<Tracked> b(7);
        a = std::move(b);
    }
    EXPECT_EQ(Tracked::dtors, 10) << "elements were destroyed twice, or not at all";
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// Self move-assignment
// ---------------------------------------------------------------------------
 
TEST(MoveAssign, SelfMoveDoesNotDestroyTheVector) {
    // Without the this == &other guard, destroy_all() runs first and the
    // vector annihilates itself. It happens for real inside sort/remove.
    vector<int> a(4, 3);
    a = std::move(a);
 
    ASSERT_EQ(a.size(), 4u);
    for (vector<int>::size_type i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], 3);
}
 
TEST(MoveAssign, SelfMoveThroughAReferenceIsSafe) {
    Tracked::reset();
    {
        vector<Tracked> a(5);
        vector<Tracked>& alias = a;
        a = std::move(alias);
        EXPECT_EQ(Tracked::live, 5) << "self-move destroyed the elements";
        EXPECT_EQ(a.size(), 5u);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveAssign, SelfMoveLeavesADestructibleObject) {
    // ASan: a self-move that frees the buffer then keeps the pointer is a
    // double free at scope exit.
    {
        vector<Owning> a(4, Owning("payload"));
        a = std::move(a);
    }
    SUCCEED();
}
 
// ---------------------------------------------------------------------------
// Edge cases and lifetimes
// ---------------------------------------------------------------------------
 
TEST(MoveAssign, MovingIntoAnEmptyVector) {
    vector<int> a;
    vector<int> b(4, 2);
    a = std::move(b);
    ASSERT_EQ(a.size(), 4u);
    EXPECT_EQ(a[3], 2);
    EXPECT_EQ(b.size(), 0u);
}
 
TEST(MoveAssign, MovingAnEmptyVectorIn) {
    Tracked::reset();
    {
        vector<Tracked> a(5);
        vector<Tracked> empty;
        a = std::move(empty);
        EXPECT_EQ(a.size(), 0u);
        EXPECT_EQ(Tracked::live, 0) << "the target's elements were not destroyed";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(MoveAssign, MovedFromTargetCanBeAssignedAgain) {
    // "Valid but unspecified" means still usable, not merely destructible.
    vector<int> a(3, 1);
    vector<int> b(4, 2);
    a = std::move(b);
 
    vector<int> c(2, 5);
    b = c;
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[1], 5);
}
 
TEST(MoveAssign, SourceOutlivingTheTargetIsSafe) {
    vector<Owning> b(4, Owning("payload"));
    {
        vector<Owning> a(2, Owning("old"));
        a = std::move(b);
    }
    EXPECT_EQ(b.size(), 0u);
}
 
TEST(MoveAssign, ChainedMovesKeepOneOwner) {
    vector<int> a(3, 4);
    const int* buffer = &a[0];
 
    vector<int> b, c;
    b = std::move(a);
    c = std::move(b);
 
    EXPECT_EQ(&c[0], buffer) << "the buffer was copied somewhere in the chain";
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(c.size(), 3u);
}
 
TEST(MoveAssign, WorksForElementsThatCannotBeCopied) {
    // Budget exhausted before the move, so any element copy would throw.
    ThrowOnCopy::reset(/*budget=*/8);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> a(2, proto);
        vector<ThrowOnCopy> b(6, proto);
        ASSERT_EQ(ThrowOnCopy::budget, 0);
 
        EXPECT_NO_THROW({ a = std::move(b); });
        EXPECT_EQ(a.size(), 6u);
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(MoveAssign, WorksForElementsThatCannotBeAssigned) {
    // No element assignment happens either -- only three scalars move.
    ThrowOnAssign::reset(/*budget=*/0);
    {
        vector<ThrowOnAssign> a(3, ThrowOnAssign(1));
        vector<ThrowOnAssign> b(3, ThrowOnAssign(2));
 
        EXPECT_NO_THROW({ a = std::move(b); });
        EXPECT_EQ(a.size(), 3u);
    }
    EXPECT_EQ(ThrowOnAssign::live, 0);
}
 
TEST(MoveAssign, NestedVectors) {
    vector<vector<int>> a(1, vector<int>(2, 1));
    vector<vector<int>> b(2, vector<int>(3, 7));
    const int* inner = &b[0][0];
 
    a = std::move(b);
 
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[1][2], 7);
    EXPECT_EQ(&a[0][0], inner) << "the inner buffers were reallocated";
    EXPECT_EQ(b.size(), 0u);
}
 
TEST(MoveAssign, RepeatedMovesDoNotLeak) {
    // ASan: a missing destroy_all() on the target shows up as 200 leaked buffers.
    vector<Owning> a;
    for (int i = 0; i < 200; ++i) {
        vector<Owning> b(static_cast<vector<Owning>::size_type>(i % 20 + 1),
                         Owning("payload"));
        a = std::move(b);
    }
    SUCCEED();
}
 

// ---------------------------------------------------------------------------
// operator[]
// ---------------------------------------------------------------------------
 
TEST(Subscript, ReadsEveryElement) {
    vector<int> v(5, 0);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) EXPECT_EQ(v[i], static_cast<int>(i));
}
 
TEST(Subscript, ReturnsAReference) {
    vector<int> v(3, 1);
    v[1] = 77;
    EXPECT_EQ(v[1], 77) << "operator[] returned by value, so the write was discarded";
    static_assert(std::is_same<decltype(std::declval<vector<int>&>()[0]), int&>::value,
                  "non-const operator[] must return T&");
}
 
TEST(Subscript, ConstOverloadReturnsAConstReference) {
    const vector<int> v(3, 5);
    const int& r = v[0];
    EXPECT_EQ(r, 5);
    EXPECT_EQ(&r, &v[0]) << "const operator[] handed back a temporary copy";
    static_assert(std::is_same<decltype(std::declval<const vector<int>&>()[0]),
                               const int&>::value,
                  "const operator[] must return const T&");
}
 
TEST(Subscript, ElementsAreContiguous) {
    vector<int> v(4, 0);
    EXPECT_EQ(&v[0] + 1, &v[1]);
    EXPECT_EQ(&v[0] + 3, &v[3]);
}
 
TEST(Subscript, LastValidIndexIsSizeMinusOne) {
    vector<int> v(3, 0);
    v[2] = 9;
    EXPECT_EQ(v[2], 9);
}
 
// ---------------------------------------------------------------------------
// at
// ---------------------------------------------------------------------------
 
TEST(At, ReadsInRangeElements) {
    vector<int> v(4, 7);
    EXPECT_EQ(v.at(0), 7);
    EXPECT_EQ(v.at(3), 7);
}
 
TEST(At, ReturnsAReference) {
    vector<int> v(3, 1);
    v.at(1) = 42;
    EXPECT_EQ(v[1], 42);
}
 
TEST(At, ConstOverloadReturnsAConstReference) {
    const vector<int> v(3, 5);
    const int& r = v.at(2);
    EXPECT_EQ(&r, &v[2]);
}
 
TEST(At, ThrowsOnIndexEqualToSize) {
    // The boundary case: size_ is one past the last valid index, so this must
    // throw. A `>` check instead of `>=` lets it through and returns raw memory.
    vector<int> v(3, 1);
    EXPECT_THROW({ v.at(3); }, std::out_of_range);
}
 
TEST(At, ThrowsWellPastTheEnd) {
    vector<int> v(3, 1);
    EXPECT_THROW({ v.at(100); }, std::out_of_range);
}
 
TEST(At, ThrowsOnEmptyVector) {
    vector<int> v;
    EXPECT_THROW({ v.at(0); }, std::out_of_range);
}
 
TEST(At, ConstOverloadThrowsToo) {
    const vector<int> v(2, 1);
    EXPECT_THROW({ v.at(2); }, std::out_of_range);
}
 
TEST(At, NegativeIndexWrapsAndThrows) {
    // size_type is unsigned, so -1 becomes SIZE_MAX and the bounds check
    // catches it. No separate lower-bound test is needed.
    vector<int> v(3, 1);
    const auto bad = static_cast<vector<int>::size_type>(-1);
    EXPECT_THROW({ v.at(bad); }, std::out_of_range);
}
 
TEST(At, DoesNotThrowForEveryValidIndex) {
    vector<int> v(5, 2);
    for (vector<int>::size_type i = 0; i < v.size(); ++i)
        EXPECT_NO_THROW({ v.at(i); }) << "index " << i << " was rejected";
}
 
// ---------------------------------------------------------------------------
// front / back
// ---------------------------------------------------------------------------
 
TEST(FrontBack, ReturnTheFirstAndLastElements) {
    vector<int> v(4, 0);
    v[0] = 10;
    v[3] = 40;
    EXPECT_EQ(v.front(), 10);
    EXPECT_EQ(v.back(), 40) << "back() is off by one -- data_[size_] is past the end";
}
 
TEST(FrontBack, AliasTheRightAddresses) {
    // The sharpest check: back() must be &v[size-1], not &v[size].
    vector<int> v(5, 1);
    EXPECT_EQ(&v.front(), &v[0]);
    EXPECT_EQ(&v.back(), &v[4]);
    EXPECT_EQ(&v.back(), &v[0] + v.size() - 1);
}
 
TEST(FrontBack, AreWritable) {
    vector<int> v(3, 0);
    v.front() = 1;
    v.back() = 3;
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[2], 3);
}
 
TEST(FrontBack, ConstOverloadsReturnConstReferences) {
    const vector<int> v(3, 8);
    const int& f = v.front();
    const int& b = v.back();
    EXPECT_EQ(&f, &v[0]);
    EXPECT_EQ(&b, &v[2]);
}
 
TEST(FrontBack, AreTheSameElementForASingleton) {
    vector<int> v(1, 5);
    EXPECT_EQ(&v.front(), &v.back());
    EXPECT_EQ(v.back(), 5);
}
 
TEST(FrontBack, BackReadsALiveObject) {
    // With the off-by-one, back() returns raw memory past the last element.
    // Reading a Tracked there would touch an object that was never constructed;
    // comparing values catches it without relying on UB being observable.
    Tracked::reset();
    {
        vector<Tracked> v(3);
        v[2].value = 99;
        EXPECT_EQ(v.back().value, 99) << "back() is not the last element";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// data
// ---------------------------------------------------------------------------
 
TEST(Data, PointsAtTheFirstElement) {
    vector<int> v(3, 4);
    EXPECT_EQ(v.data(), &v[0]);
}
 
TEST(Data, IsUsableAsARawArray) {
    vector<int> v(4, 0);
    int* p = v.data();
    for (int i = 0; i < 4; ++i) p[i] = i * 10;
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[3], 30);
}
 
TEST(Data, ConstOverloadReturnsAConstPointer) {
    const vector<int> v(3, 6);
    const int* p = v.data();
    EXPECT_EQ(p, &v[0]);
    EXPECT_EQ(p[2], 6);
    static_assert(std::is_same<decltype(std::declval<const vector<int>&>().data()),
                               const int*>::value,
                  "const data() must return const T*");
}
 
TEST(Data, IsNullForADefaultConstructedVector) {
    vector<int> v;
    EXPECT_EQ(v.data(), nullptr);
}
 
TEST(Data, SurvivesACopy) {
    vector<int> a(3, 1);
    vector<int> b(a);
    EXPECT_NE(a.data(), b.data()) << "the copy shares the source's buffer";
}
 
TEST(Data, FollowsTheBufferThroughAMove) {
    vector<int> a(3, 1);
    const int* buffer = a.data();
    vector<int> b(std::move(a));
    EXPECT_EQ(b.data(), buffer);
    EXPECT_EQ(a.data(), nullptr) << "the moved-from vector still points at the buffer";
}
 
// ---------------------------------------------------------------------------
// Consistency across accessors
// ---------------------------------------------------------------------------
 
TEST(ElementAccess, AllAccessorsAgree) {
    vector<int> v(5, 0);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i) + 1;
 
    EXPECT_EQ(&v.at(0), &v[0]);
    EXPECT_EQ(&v.at(4), &v[4]);
    EXPECT_EQ(&v.front(), v.data());
    EXPECT_EQ(&v.back(), v.data() + v.size() - 1);
}
 
TEST(ElementAccess, WorkOnAConstVector) {
    // Fails to compile if any const overload is missing.
    const vector<int> v(3, 2);
    EXPECT_EQ(v[0], 2);
    EXPECT_EQ(v.at(1), 2);
    EXPECT_EQ(v.front(), 2);
    EXPECT_EQ(v.back(), 2);
    EXPECT_EQ(v.data()[2], 2);
}
 


// ---------------------------------------------------------------------------
// Every route into the state produces the same state
// ---------------------------------------------------------------------------
 
TEST(Unallocated, DefaultConstructed) {
    vector<int> v;
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
    EXPECT_EQ(v.data(), nullptr);
}
 
TEST(Unallocated, CountZero) {
    vector<int> v(0);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u) << "capacity_ was set before the allocation was skipped";
    EXPECT_EQ(v.data(), nullptr) << "allocate_raw(0) must not allocate";
}
 
TEST(Unallocated, FillWithCountZero) {
    vector<int> v(0, 42);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
    EXPECT_EQ(v.data(), nullptr);
}
 
TEST(Unallocated, MovedFrom) {
    vector<int> a(4, 1);
    vector<int> b(std::move(a));
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.capacity(), 0u);
    EXPECT_EQ(a.data(), nullptr) << "the moved-from vector still points at the buffer";
}
 
TEST(Unallocated, MoveAssignedFrom) {
    vector<int> a(4, 1);
    vector<int> b;
    b = std::move(a);
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.capacity(), 0u);
    EXPECT_EQ(a.data(), nullptr);
}
 
TEST(Unallocated, AssignedAnEmptyVectorKeepsItsBuffer) {
    // The one case that does NOT become unallocated: assignment reuses the
    // buffer, so size_ drops to 0 but capacity_ and data_ survive.
    vector<int> a(5, 1);
    vector<int> empty;
    a = empty;
    EXPECT_EQ(a.size(), 0u);
    EXPECT_GE(a.capacity(), 5u) << "assignment should not release capacity";
    EXPECT_NE(a.data(), nullptr);
}
 
// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
 
TEST(Unallocated, DestroyingIsSafe) {
    // free(nullptr) is a defined no-op and the destroy loop runs zero times.
    { vector<int> v; }
    { vector<int> v(0); }
    { vector<Owning> v; }
    SUCCEED();
}
 
TEST(Unallocated, DestroyingAMovedFromVectorIsSafe) {
    // ASan: if data_ was not nulled, this is a double free.
    {
        vector<Owning> a(4, Owning("payload"));
        vector<Owning> b(std::move(a));
    }
    SUCCEED();
}
 
// ---------------------------------------------------------------------------
// Copying and moving out of the state
// ---------------------------------------------------------------------------
 
TEST(Unallocated, CanBeCopyConstructedFrom) {
    vector<int> a;
    vector<int> b(a);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.capacity(), 0u);
    EXPECT_EQ(b.data(), nullptr);
}
 
TEST(Unallocated, CanBeMoveConstructedFrom) {
    vector<int> a;
    vector<int> b(std::move(a));
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.data(), nullptr);
    EXPECT_EQ(a.size(), 0u);
}
 
TEST(Unallocated, CopyingAMovedFromVectorIsSafe) {
    vector<int> a(3, 1);
    vector<int> b(std::move(a));
    vector<int> c(a);  // copy of the hollowed-out source
    EXPECT_EQ(c.size(), 0u);
    EXPECT_EQ(c.data(), nullptr);
}
 
TEST(Unallocated, CopyConstructingFromEmptyDoesNoElementWork) {
    Tracked::reset();
    {
        vector<Tracked> a;
        vector<Tracked> b(a);
        EXPECT_EQ(Tracked::copy_ctors, 0);
        EXPECT_EQ(Tracked::live, 0);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// Assignment into and out of the state
// ---------------------------------------------------------------------------
 
TEST(Unallocated, CanBeCopyAssignedInto) {
    // The reallocating branch starting from capacity_ == 0.
    vector<int> a;
    vector<int> b(3, 7);
    a = b;
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[2], 7);
    EXPECT_NE(a.data(), nullptr);
}
 
TEST(Unallocated, CanBeMoveAssignedInto) {
    vector<int> a;
    vector<int> b(3, 7);
    a = std::move(b);
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[2], 7);
    EXPECT_EQ(b.size(), 0u);
}
 
TEST(Unallocated, CanBeCopyAssignedFrom) {
    // Shrink-to-nothing: destroys the target's elements, keeps its buffer.
    Tracked::reset();
    {
        vector<Tracked> a(5);
        vector<Tracked> empty;
        a = empty;
        EXPECT_EQ(a.size(), 0u);
        EXPECT_EQ(Tracked::live, 0) << "the target's elements were not destroyed";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(Unallocated, CanBeMoveAssignedFrom) {
    Tracked::reset();
    {
        vector<Tracked> a(5);
        vector<Tracked> empty;
        a = std::move(empty);
        EXPECT_EQ(a.size(), 0u);
        EXPECT_EQ(a.data(), nullptr) << "the target kept a buffer it no longer owns";
        EXPECT_EQ(Tracked::live, 0);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(Unallocated, EmptyToEmptyAssignment) {
    vector<int> a, b;
    a = b;
    EXPECT_EQ(a.size(), 0u);
    a = std::move(b);
    EXPECT_EQ(a.size(), 0u);
}
 
TEST(Unallocated, SelfAssignmentIsSafe) {
    vector<int> a;
    a = a;
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.data(), nullptr);
}
 
TEST(Unallocated, SelfMoveAssignmentIsSafe) {
    vector<int> a;
    a = std::move(a);
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.data(), nullptr);
}
 
TEST(Unallocated, MovedFromVectorCanBeReused) {
    // "Valid but unspecified" means usable, not merely destructible.
    vector<int> a(3, 1);
    vector<int> b(std::move(a));
 
    vector<int> c(2, 5);
    a = c;
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[1], 5);
}
 
// ---------------------------------------------------------------------------
// Accessors on the empty state
// ---------------------------------------------------------------------------
 
TEST(Unallocated, AtAlwaysThrows) {
    vector<int> v;
    EXPECT_THROW({ v.at(0); }, std::out_of_range);
    EXPECT_THROW({ v.at(1); }, std::out_of_range);
}
 
TEST(Unallocated, ConstAtAlsoThrows) {
    const vector<int> v;
    EXPECT_THROW({ v.at(0); }, std::out_of_range);
}
 
TEST(Unallocated, SizeAndCapacityWorkOnConst) {
    const vector<int> v;
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
    EXPECT_EQ(v.data(), nullptr);
}
 
TEST(Unallocated, IterationRangeIsEmpty) {
    // The common loop shape must terminate immediately without dereferencing.
    vector<int> v;
    int count = 0;
    for (vector<int>::size_type i = 0; i < v.size(); ++i) ++count;
    EXPECT_EQ(count, 0);
 
    // Pointer arithmetic on a null data() with a zero count: begin == end.
    EXPECT_EQ(v.data(), v.data() + v.size());
}
 
// ---------------------------------------------------------------------------
// Failed construction
// ---------------------------------------------------------------------------
 
TEST(Unallocated, ThrowingConstructionLeavesNoObject) {
    // A constructor that throws never produces an object, so there is nothing
    // to inspect afterwards -- only the absence of a leak. ASan checks that.
    ThrowOnCopy::reset(/*budget=*/2);
    ThrowOnCopy proto(1);
    EXPECT_THROW({ vector<ThrowOnCopy> v(10, proto); }, std::runtime_error);
    EXPECT_EQ(ThrowOnCopy::live, 1) << "cleanup on the throwing path was wrong";
}
 
TEST(Unallocated, StaysUsableAfterAFailedAssignment) {
    // Unlike a constructor, a failed assignment leaves a live object. An empty
    // target must still be empty and usable afterwards.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> a;
        vector<ThrowOnCopy> b(10, proto);
 
        ThrowOnCopy::budget = 3;
        EXPECT_THROW({ a = b; }, std::runtime_error);
 
        EXPECT_EQ(a.size(), 0u) << "the empty target was corrupted";
        EXPECT_EQ(a.data(), nullptr);
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 

// ---------------------------------------------------------------------------
// Element types that report move vs copy
// ---------------------------------------------------------------------------
 
// noexcept move: move_if_noexcept must choose the move constructor.
struct Movable {
    static int copies;
    static int moves;
    static int live;
 
    int value;
 
    explicit Movable(int v = 0) : value(v) { ++live; }
    Movable(const Movable& o) : value(o.value) { ++copies; ++live; }
    Movable(Movable&& o) noexcept : value(o.value) { o.value = -1; ++moves; ++live; }
    Movable& operator=(const Movable& o) { value = o.value; return *this; }
    ~Movable() { --live; }
 
    static void reset() { copies = moves = live = 0; }
};
 
int Movable::copies = 0;
int Movable::moves = 0;
int Movable::live = 0;
 
// Move constructor NOT marked noexcept: move_if_noexcept must fall back to
// copying, because a throw partway through a transfer cannot be rolled back.
struct ThrowingMove {
    static int copies;
    static int moves;
    static int live;
 
    int value;
 
    explicit ThrowingMove(int v = 0) : value(v) { ++live; }
    ThrowingMove(const ThrowingMove& o) : value(o.value) { ++copies; ++live; }
    ThrowingMove(ThrowingMove&& o) : value(o.value) { ++moves; ++live; }
    ThrowingMove& operator=(const ThrowingMove& o) { value = o.value; return *this; }
    ~ThrowingMove() { --live; }
 
    static void reset() { copies = moves = live = 0; }
};
 
int ThrowingMove::copies = 0;
int ThrowingMove::moves = 0;
int ThrowingMove::live = 0;
 
// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------
 
TEST(Reserve, IncreasesCapacity) {
    vector<int> v(3, 1);
    v.reserve(50);
    EXPECT_GE(v.capacity(), 50u);
}
 
TEST(Reserve, PreservesSize) {
    // destroy_all() zeroes size_, so reserve must restore it afterwards.
    vector<int> v(3, 1);
    v.reserve(50);
    EXPECT_EQ(v.size(), 3u) << "reserve changed size(); capacity and size are independent";
}
 
TEST(Reserve, PreservesElementValues) {
    vector<int> v(4, 0);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i) * 10;
 
    v.reserve(100);
 
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[3], 30);
}
 
TEST(Reserve, ReallocatesToANewBuffer) {
    vector<int> v(3, 1);
    const int* before = v.data();
    v.reserve(100);
    EXPECT_NE(v.data(), before) << "capacity grew without a new allocation";
}
 
TEST(Reserve, ElementsRemainContiguous) {
    vector<int> v(4, 7);
    v.reserve(64);
    EXPECT_EQ(&v[0] + 3, &v[3]);
    EXPECT_EQ(v.data(), &v[0]);
}
 
TEST(Reserve, OnAnEmptyVectorAllocates) {
    vector<int> v;
    ASSERT_EQ(v.data(), nullptr);
 
    v.reserve(10);
 
    EXPECT_GE(v.capacity(), 10u);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_NE(v.data(), nullptr);
}
 
TEST(Reserve, AccessorsWorkAfterwards) {
    vector<int> v(3, 5);
    v.reserve(64);
    EXPECT_EQ(v.front(), 5);
    EXPECT_EQ(v.back(), 5);
    EXPECT_EQ(v.at(2), 5);
    EXPECT_FALSE(v.empty());
}
 
// ---------------------------------------------------------------------------
// No-op cases: reserve must never shrink
// ---------------------------------------------------------------------------
 
TEST(Reserve, SmallerThanCapacityIsANoOp) {
    vector<int> v(10, 1);
    const int* before = v.data();
    const auto cap = v.capacity();
 
    v.reserve(2);
 
    EXPECT_EQ(v.capacity(), cap) << "reserve shrank the capacity";
    EXPECT_EQ(v.data(), before) << "reserve reallocated for a smaller request";
    EXPECT_EQ(v.size(), 10u);
}
 
TEST(Reserve, EqualToCapacityIsANoOp) {
    // The <= boundary: reserve(capacity_) must not reallocate.
    vector<int> v(10, 1);
    const int* before = v.data();
 
    v.reserve(v.capacity());
 
    EXPECT_EQ(v.data(), before) << "reserve(capacity()) reallocated pointlessly";
}
 
TEST(Reserve, ZeroIsANoOp) {
    vector<int> v(5, 1);
    const int* before = v.data();
    v.reserve(0);
    EXPECT_EQ(v.data(), before);
    EXPECT_EQ(v.size(), 5u);
}
 
TEST(Reserve, NoOpDoesNoElementWork) {
    Tracked::reset();
    {
        vector<Tracked> v(5);
        Tracked::copy_ctors = 0;
        Tracked::dtors = 0;
 
        v.reserve(2);
 
        EXPECT_EQ(Tracked::copy_ctors, 0);
        EXPECT_EQ(Tracked::dtors, 0) << "a no-op reserve destroyed elements";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// How elements get transferred
// ---------------------------------------------------------------------------
 
TEST(Reserve, MovesElementsWhenTheMoveIsNoexcept) {
    // The payoff for marking move constructors noexcept.
    Movable::reset();
    {
        vector<Movable> v(6, Movable(1));
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.reserve(100);
 
        EXPECT_EQ(Movable::moves, 6) << "elements were not moved into the new buffer";
        EXPECT_EQ(Movable::copies, 0) << "reserve copied despite a noexcept move";
    }
    EXPECT_EQ(Movable::live, 0);
}
 
TEST(Reserve, CopiesElementsWhenTheMoveMayThrow) {
    // move_if_noexcept falls back to copying, because a throw mid-transfer
    // cannot be undone and the strong guarantee would be lost.
    ThrowingMove::reset();
    {
        vector<ThrowingMove> v(6, ThrowingMove(1));
        ThrowingMove::copies = 0;
        ThrowingMove::moves = 0;
 
        v.reserve(100);
 
        EXPECT_EQ(ThrowingMove::copies, 6) << "a throwing move was used for the transfer";
        EXPECT_EQ(ThrowingMove::moves, 0);
    }
    EXPECT_EQ(ThrowingMove::live, 0);
}
 
TEST(Reserve, DoesNotConstructBeyondSize) {
    // Slots between size_ and capacity_ stay raw. Constructing them would make
    // reserve behave like resize.
    Tracked::reset();
    {
        vector<Tracked> v(3);
        v.reserve(100);
        EXPECT_EQ(Tracked::live, 3) << "reserve constructed elements in the spare capacity";
        EXPECT_EQ(v.size(), 3u);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(Reserve, DestroysEachElementExactlyOnce) {
    Tracked::reset();
    {
        vector<Tracked> v(8);
        v.reserve(200);
    }
    // 8 originals destroyed after the transfer, 8 copies destroyed at scope exit.
    EXPECT_EQ(Tracked::live, 0) << "elements were leaked or destroyed twice";
}
 
TEST(Reserve, ReleasesTheOldBuffer) {
    // ASan: the pre-reserve buffer and its strings must be freed exactly once.
    vector<Owning> v(10, Owning("payload"));
    v.reserve(500);
    EXPECT_EQ(v.size(), 10u);
}
 
TEST(Reserve, RepeatedGrowthDoesNotLeak) {
    vector<Owning> v(4, Owning("payload"));
    for (vector<Owning>::size_type cap = 8; cap <= 4096; cap *= 2) {
        v.reserve(cap);
        EXPECT_EQ(v.size(), 4u);
    }
    SUCCEED();
}
 
// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------
 
TEST(Reserve, ThrowingTransferLeavesTheVectorUntouched) {
    // The strong guarantee: build the new buffer first, and only commit once
    // every element has arrived. ThrowOnCopy has no move ctor, so the transfer
    // copies and the budget applies.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> v(10, proto);
        const ThrowOnCopy* before = v.data();
        const auto cap = v.capacity();
        const int live_before = ThrowOnCopy::live;
 
        ThrowOnCopy::budget = 4;  // fails on the 5th of 10 transfers
        EXPECT_THROW({ v.reserve(500); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 10u) << "size changed after a failed reserve";
        EXPECT_EQ(v.capacity(), cap) << "capacity was committed before the transfer finished";
        EXPECT_EQ(v.data(), before) << "the original buffer was released";
        EXPECT_EQ(ThrowOnCopy::live, live_before) << "the partial buffer leaked";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(Reserve, VectorStillUsableAfterAFailedReserve) {
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(7);
        vector<ThrowOnCopy> v(5, proto);
 
        ThrowOnCopy::budget = 2;
        EXPECT_THROW({ v.reserve(100); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 5u);
        EXPECT_EQ(v[4].value, 7) << "the original elements were damaged";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(Reserve, AboveMaxSizeThrowsLengthError) {
   vector<int> v(3, 1);
   EXPECT_THROW({ v.reserve(v.max_size() + 1); }, std::length_error);
   EXPECT_EQ(v.size(), 3u) << "the vector was modified before the check";
}
 
TEST(Reserve, HugeButRepresentableRequestThrowsBadAlloc) {
    // Under the request limit but far beyond available memory. Needs
    // ASAN_OPTIONS=allocator_may_return_null=1 to reach the null check.
    vector<int> v(3, 1);
    const auto huge = std::numeric_limits<vector<int>::size_type>::max() / sizeof(int) / 2;
    EXPECT_THROW({ v.reserve(huge); }, std::bad_alloc);
    EXPECT_EQ(v.size(), 3u);
}
 
// ---------------------------------------------------------------------------
// Interaction with the rest of the class
// ---------------------------------------------------------------------------
 
TEST(Reserve, ReservedCapacitySurvivesAssignment) {
    // Copy assignment reuses the buffer when capacity allows, so a reserve
    // beforehand should prevent reallocation.
    vector<int> a;
    a.reserve(100);
    const int* buffer = a.data();
 
    vector<int> b(50, 3);
    a = b;
 
    EXPECT_EQ(a.data(), buffer) << "assignment reallocated despite reserved capacity";
    EXPECT_EQ(a.size(), 50u);
}
 
TEST(Reserve, CapacityIsNotCarriedIntoACopy) {
    // The copy allocates other.size(), not other.capacity().
    vector<int> a(3, 1);
    a.reserve(500);
 
    vector<int> b(a);
 
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b.capacity(), 3u) << "the copy allocated the source's spare capacity";
}
 
TEST(Reserve, CapacityIsCarriedThroughAMove) {
    vector<int> a(3, 1);
    a.reserve(500);
    const auto cap = a.capacity();
 
    vector<int> b(std::move(a));
 
    EXPECT_EQ(b.capacity(), cap) << "the move did not transfer capacity";
    EXPECT_EQ(a.capacity(), 0u);
}
 

// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------
 
TEST(ShrinkToFit, ReducesCapacityToSize) {
    vector<int> v(3, 1);
    v.reserve(500);
    ASSERT_GE(v.capacity(), 500u);
 
    v.shrink_to_fit();
 
    EXPECT_EQ(v.capacity(), 3u) << "capacity was not reduced to size";
    EXPECT_EQ(v.size(), 3u);
}
 
TEST(ShrinkToFit, PreservesElementValues) {
    vector<int> v(4, 0);
    for (vector<int>::size_type i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i) * 10;
    v.reserve(500);
 
    v.shrink_to_fit();
 
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[3], 30);
}
 
TEST(ShrinkToFit, ReallocatesToANewBuffer) {
    // Capacity is a property of the allocation, so shrinking must actually
    // allocate a smaller block -- it cannot just lower the member.
    vector<int> v(3, 1);
    v.reserve(500);
    const int* before = v.data();
 
    v.shrink_to_fit();
 
    EXPECT_NE(v.data(), before) << "capacity changed without a new allocation";
}
 
TEST(ShrinkToFit, ElementsRemainContiguous) {
    vector<int> v(4, 7);
    v.reserve(200);
    v.shrink_to_fit();
    EXPECT_EQ(&v[0] + 3, &v[3]);
    EXPECT_EQ(v.data(), &v[0]);
}
 
TEST(ShrinkToFit, AccessorsWorkAfterwards) {
    vector<int> v(3, 5);
    v.reserve(100);
    v.shrink_to_fit();
    EXPECT_EQ(v.front(), 5);
    EXPECT_EQ(v.back(), 5);
    EXPECT_EQ(v.at(2), 5);
    EXPECT_FALSE(v.empty());
}
 
TEST(ShrinkToFit, WorksAfterShrinkingViaAssignment) {
    // The usual way spare capacity appears: assign a smaller vector.
    vector<int> v(100, 1);
    vector<int> small(3, 8);
    v = small;
    ASSERT_EQ(v.size(), 3u);
    ASSERT_GE(v.capacity(), 100u);
 
    v.shrink_to_fit();
 
    EXPECT_EQ(v.capacity(), 3u);
    EXPECT_EQ(v[2], 8);
}
 
// ---------------------------------------------------------------------------
// No-op and empty cases
// ---------------------------------------------------------------------------
 
TEST(ShrinkToFit, IsANoOpWhenAlreadyExact) {
    vector<int> v(5, 1);
    ASSERT_EQ(v.capacity(), v.size());
    const int* before = v.data();
 
    v.shrink_to_fit();
 
    EXPECT_EQ(v.data(), before) << "reallocated despite capacity already matching size";
    EXPECT_EQ(v.size(), 5u);
}
 
TEST(ShrinkToFit, NoOpDoesNoElementWork) {
    Tracked::reset();
    {
        vector<Tracked> v(5);
        Tracked::copy_ctors = 0;
        Tracked::dtors = 0;
 
        v.shrink_to_fit();
 
        EXPECT_EQ(Tracked::copy_ctors, 0);
        EXPECT_EQ(Tracked::dtors, 0) << "a no-op shrink destroyed elements";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(ShrinkToFit, OnAnUnallocatedVectorIsANoOp) {
    vector<int> v;
    v.shrink_to_fit();
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);
    EXPECT_EQ(v.data(), nullptr);
}
 
TEST(ShrinkToFit, OnAnEmptyVectorWithCapacityReleasesTheBuffer) {
    // size_ == 0 and capacity_ > 0: reallocate(0) must free the old block and
    // leave the vector fully unallocated. Relies on allocate_raw(0) returning
    // nullptr rather than allocating.
    vector<int> v;
    v.reserve(100);
    ASSERT_NE(v.data(), nullptr);
 
    v.shrink_to_fit();
 
    EXPECT_EQ(v.capacity(), 0u);
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.data(), nullptr) << "the oversized buffer was not released";
}
 
TEST(ShrinkToFit, EmptiedVectorReleasesEverything) {
    Tracked::reset();
    {
        vector<Tracked> v(20);
        vector<Tracked> empty;
        v = empty;              // size 0, capacity still 20
        ASSERT_EQ(Tracked::live, 0);
 
        v.shrink_to_fit();
 
        EXPECT_EQ(v.capacity(), 0u);
        EXPECT_EQ(v.data(), nullptr);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// How elements get transferred
// ---------------------------------------------------------------------------
 
TEST(ShrinkToFit, MovesElementsWhenTheMoveIsNoexcept) {
    Movable::reset();
    {
        vector<Movable> v(6, Movable(1));
        v.reserve(500);
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.shrink_to_fit();
 
        EXPECT_EQ(Movable::moves, 6);
        EXPECT_EQ(Movable::copies, 0) << "copied despite a noexcept move";
    }
    EXPECT_EQ(Movable::live, 0);
}
 
TEST(ShrinkToFit, CopiesElementsWhenTheMoveMayThrow) {
    ThrowingMove::reset();
    {
        vector<ThrowingMove> v(6, ThrowingMove(1));
        v.reserve(500);
        ThrowingMove::copies = 0;
        ThrowingMove::moves = 0;
 
        v.shrink_to_fit();
 
        EXPECT_EQ(ThrowingMove::copies, 6) << "a throwing move was used for the transfer";
        EXPECT_EQ(ThrowingMove::moves, 0);
    }
    EXPECT_EQ(ThrowingMove::live, 0);
}
 
TEST(ShrinkToFit, TransfersExactlySizeElements) {
    // The slots between size_ and capacity_ hold no objects, so nothing there
    // is transferred or destroyed.
    Tracked::reset();
    {
        vector<Tracked> v(3);
        v.reserve(100);
        Tracked::copy_ctors = 0;
 
        v.shrink_to_fit();
 
        EXPECT_EQ(Tracked::copy_ctors, 3) << "transferred the wrong number of elements";
        EXPECT_EQ(Tracked::live, 3);
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
TEST(ShrinkToFit, DestroysEachElementExactlyOnce) {
    Tracked::reset();
    {
        vector<Tracked> v(8);
        v.reserve(500);
        v.shrink_to_fit();
    }
    EXPECT_EQ(Tracked::live, 0) << "elements were leaked or destroyed twice";
}
 
TEST(ShrinkToFit, ReleasesTheOldBuffer) {
    // ASan: the oversized buffer and its strings must be freed exactly once.
    vector<Owning> v(10, Owning("payload"));
    v.reserve(1000);
    v.shrink_to_fit();
    EXPECT_EQ(v.size(), 10u);
}
 
TEST(ShrinkToFit, RepeatedGrowShrinkCyclesDoNotLeak) {
    vector<Owning> v(4, Owning("payload"));
    for (int i = 0; i < 50; ++i) {
        v.reserve(1000);
        v.shrink_to_fit();
        EXPECT_EQ(v.size(), 4u);
        EXPECT_EQ(v.capacity(), 4u);
    }
    SUCCEED();
}
 
// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------
 
TEST(ShrinkToFit, ThrowingTransferLeavesTheVectorUntouched) {
    // Same strong guarantee as reserve: the new buffer is built before the old
    // one is released, so a throw must change nothing.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> v(10, proto);
        v.reserve(500);
        const ThrowOnCopy* before = v.data();
        const auto cap = v.capacity();
        const int live_before = ThrowOnCopy::live;
 
        ThrowOnCopy::budget = 4;  // fails on the 5th of 10 transfers
        EXPECT_THROW({ v.shrink_to_fit(); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 10u);
        EXPECT_EQ(v.capacity(), cap) << "capacity was committed before the transfer finished";
        EXPECT_EQ(v.data(), before) << "the original buffer was released";
        EXPECT_EQ(ThrowOnCopy::live, live_before) << "the partial buffer leaked";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(ShrinkToFit, VectorStillUsableAfterAFailedShrink) {
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(7);
        vector<ThrowOnCopy> v(5, proto);
        v.reserve(200);
 
        ThrowOnCopy::budget = 2;
        EXPECT_THROW({ v.shrink_to_fit(); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 5u);
        EXPECT_EQ(v[4].value, 7) << "the original elements were damaged";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
// ---------------------------------------------------------------------------
// Interaction with reserve and the rest of the class
// ---------------------------------------------------------------------------
 
TEST(ShrinkToFit, ReserveAfterShrinkStillGrows) {
    vector<int> v(3, 1);
    v.reserve(500);
    v.shrink_to_fit();
    ASSERT_EQ(v.capacity(), 3u);
 
    v.reserve(50);
 
    EXPECT_GE(v.capacity(), 50u);
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[2], 1);
}
 
TEST(ShrinkToFit, ReserveDoesNotUndoIt) {
    // reserve must never shrink, so a smaller request after shrinking is a no-op.
    vector<int> v(3, 1);
    v.reserve(500);
    v.shrink_to_fit();
    const int* before = v.data();
 
    v.reserve(2);
 
    EXPECT_EQ(v.data(), before);
    EXPECT_EQ(v.capacity(), 3u);
}
 
TEST(ShrinkToFit, CopyOfAShrunkVectorIsUnaffected) {
    vector<int> a(3, 1);
    a.reserve(500);
    vector<int> b(a);          // the copy already allocates only size()
    a.shrink_to_fit();
 
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b.capacity(), 3u);
    EXPECT_EQ(b[0], 1);
}
 
TEST(ShrinkToFit, MovedFromVectorIsAlreadyFit) {
    vector<int> a(3, 1);
    a.reserve(500);
    vector<int> b(std::move(a));
 
    a.shrink_to_fit();         // a is unallocated: nothing to do
 
    EXPECT_EQ(a.capacity(), 0u);
    EXPECT_EQ(a.data(), nullptr);
    EXPECT_EQ(b.size(), 3u);
}
 

// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------
 
TEST(PushBack, AppendsToAnEmptyVector) {
    vector<int> v;
    v.push_back(7);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 7);
    EXPECT_GE(v.capacity(), 1u);
}
 
TEST(PushBack, AppendsInOrder) {
    vector<int> v;
    for (int i = 0; i < 10; ++i) v.push_back(i);
 
    ASSERT_EQ(v.size(), 10u);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(v[i], i) << "element " << i << " is wrong";
}
 
TEST(PushBack, UpdatesBackAndFront) {
    vector<int> v;
    v.push_back(1);
    EXPECT_EQ(v.front(), 1);
    EXPECT_EQ(v.back(), 1);
 
    v.push_back(2);
    EXPECT_EQ(v.front(), 1);
    EXPECT_EQ(v.back(), 2) << "back() did not follow the new element";
}
 
TEST(PushBack, AppendsToAVectorBuiltByOtherMeans) {
    vector<int> v(3, 5);
    v.push_back(9);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[2], 5);
    EXPECT_EQ(v[3], 9);
}
 
TEST(PushBack, ElementsRemainContiguous) {
    vector<int> v;
    for (int i = 0; i < 20; ++i) v.push_back(i);
    EXPECT_EQ(&v[0] + 19, &v[19]);
    EXPECT_EQ(v.data(), &v[0]);
}
 
TEST(PushBack, ConstructsRatherThanAssigns) {
    // The slot is raw memory, so it must be placement-new'd, never assigned.
    Tracked::reset();
    {
        vector<Tracked> v;
        v.push_back(Tracked(1));
        EXPECT_EQ(Tracked::assignments, 0) << "assigned into raw memory";
    }
    EXPECT_EQ(Tracked::live, 0);
}
 
// ---------------------------------------------------------------------------
// Growth
// ---------------------------------------------------------------------------
 
TEST(PushBack, GrowsCapacityWhenFull) {
    vector<int> v;
    v.push_back(1);
    const auto cap1 = v.capacity();
    ASSERT_GE(cap1, 1u);
 
    while (v.size() < cap1) v.push_back(0);
    ASSERT_EQ(v.size(), v.capacity());
 
    v.push_back(99);
    EXPECT_GT(v.capacity(), cap1) << "capacity did not grow when full";
    EXPECT_EQ(v.back(), 99);
}
 
TEST(PushBack, DoesNotReallocateWhileCapacityRemains) {
    vector<int> v;
    v.reserve(100);
    const int* buffer = v.data();
 
    for (int i = 0; i < 100; ++i) v.push_back(i);
 
    EXPECT_EQ(v.data(), buffer) << "reallocated despite sufficient reserved capacity";
    EXPECT_EQ(v.size(), 100u);
}
 
TEST(PushBack, GrowthIsGeometricNotLinear) {
    // Counting reallocations: doubling gives O(log n) of them, growth-by-one
    // gives n. Detected by watching the buffer address change.
    vector<int> v;
    const int* last = nullptr;
    int reallocations = 0;
 
    for (int i = 0; i < 1000; ++i) {
        v.push_back(i);
        if (v.data() != last) { ++reallocations; last = v.data(); }
    }
 
    EXPECT_LT(reallocations, 30) << "capacity is not growing geometrically";
    EXPECT_EQ(v.size(), 1000u);
}
 
TEST(PushBack, GrowsFromCapacityNotSize) {
    // After a large reserve, size is small but capacity is not. Growth must be
    // driven by capacity, and must not trigger at all here.
    vector<int> v(3, 1);
    v.reserve(500);
    const int* buffer = v.data();
 
    v.push_back(4);
 
    EXPECT_EQ(v.data(), buffer) << "grew even though capacity was available";
    EXPECT_EQ(v.capacity(), 500u);
    EXPECT_EQ(v.size(), 4u);
}
 
TEST(PushBack, PreservesExistingElementsAcrossGrowth) {
    vector<int> v;
    for (int i = 0; i < 500; ++i) v.push_back(i);
 
    ASSERT_EQ(v.size(), 500u);
    EXPECT_EQ(v[0], 0) << "the first element was lost during a reallocation";
    EXPECT_EQ(v[250], 250);
    EXPECT_EQ(v[499], 499);
}
 
TEST(PushBack, MovesExistingElementsWhenGrowing) {
    // reserve uses move_if_noexcept, so a noexcept-movable element type must
    // be moved rather than copied during growth.
    Movable::reset();
    {
        vector<Movable> v;
        v.reserve(4);
        for (int i = 0; i < 4; ++i) v.push_back(Movable(i));
 
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.push_back(Movable(99));  // forces growth: 4 existing elements transfer
 
        EXPECT_EQ(Movable::copies, 0) << "existing elements were copied during growth";
        EXPECT_GE(Movable::moves, 4);
    }
    EXPECT_EQ(Movable::live, 0);
}
 
// ---------------------------------------------------------------------------
// Copy overload vs move overload
// ---------------------------------------------------------------------------
 
TEST(PushBack, LvalueSelectsTheCopyOverload) {
    Movable::reset();
    {
        vector<Movable> v;
        v.reserve(4);
        Movable m(5);
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.push_back(m);
 
        EXPECT_EQ(Movable::copies, 1) << "an lvalue was moved from";
        EXPECT_EQ(Movable::moves, 0);
        EXPECT_EQ(m.value, 5) << "the source was gutted by a copy push_back";
    }
    EXPECT_EQ(Movable::live, 0);
}
 
TEST(PushBack, RvalueSelectsTheMoveOverload) {
    Movable::reset();
    {
        vector<Movable> v;
        v.reserve(4);
        Movable m(5);
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.push_back(std::move(m));
 
        EXPECT_EQ(Movable::moves, 1) << "an rvalue selected the copy overload";
        EXPECT_EQ(Movable::copies, 0);
    }
    EXPECT_EQ(Movable::live, 0);
}
 
TEST(PushBack, RvalueMovesEvenWhenTheMoveMayThrow) {
    // move_if_noexcept belongs in reallocate, not here: there is nothing to
    // roll back, and the caller already gave up ownership. Using it would
    // silently copy every type whose move is not noexcept.
    ThrowingMove::reset();
    {
        vector<ThrowingMove> v;
        v.reserve(4);
        ThrowingMove t(5);
        ThrowingMove::copies = 0;
        ThrowingMove::moves = 0;
 
        v.push_back(std::move(t));
 
        EXPECT_EQ(ThrowingMove::moves, 1)
            << "push_back copied an rvalue because its move is not noexcept";
        EXPECT_EQ(ThrowingMove::copies, 0);
    }
    EXPECT_EQ(ThrowingMove::live, 0);
}
 
TEST(PushBack, MoveOverloadTakesOwnership) {
    vector<std::string> v;
    std::string s = "a reasonably long string that will not fit in SSO storage";
    const char* buffer = s.data();
 
    v.push_back(std::move(s));
 
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].data(), buffer) << "the string was copied rather than moved";
}
 
TEST(PushBack, TemporariesUseTheMoveOverload) {
    Movable::reset();
    {
        vector<Movable> v;
        v.reserve(4);
        Movable::copies = 0;
        Movable::moves = 0;
 
        v.push_back(Movable(7));
 
        EXPECT_EQ(Movable::copies, 0) << "a temporary was copied";
    }
    EXPECT_EQ(Movable::live, 0);
}
 
// ---------------------------------------------------------------------------
// Lifetime and cleanup
// ---------------------------------------------------------------------------
 
TEST(PushBack, EveryElementIsDestroyedExactlyOnce) {
    Tracked::reset();
    {
        vector<Tracked> v;
        for (int i = 0; i < 50; ++i) v.push_back(Tracked(i));
        EXPECT_EQ(Tracked::live, 50);
    }
    EXPECT_EQ(Tracked::live, 0) << "elements were leaked or destroyed twice";
}
 
TEST(PushBack, ReleasesResourcesAcrossManyReallocations) {
    // ASan: every intermediate buffer must be freed, and every string with it.
    vector<Owning> v;
    for (int i = 0; i < 200; ++i) v.push_back(Owning("payload"));
    EXPECT_EQ(v.size(), 200u);
}
 
TEST(PushBack, AfterClearReusesTheBuffer) {
    vector<int> v;
    v.reserve(100);
    for (int i = 0; i < 100; ++i) v.push_back(i);
    const int* buffer = v.data();
 
    v.clear();
    for (int i = 0; i < 100; ++i) v.push_back(i * 2);
 
    EXPECT_EQ(v.data(), buffer) << "reallocated despite the cleared buffer being big enough";
    EXPECT_EQ(v[99], 198);
}
 
TEST(PushBack, WorksOnAMovedFromVector) {
    vector<int> a(3, 1);
    vector<int> b(std::move(a));
 
    a.push_back(42);
 
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], 42);
}
 
// ---------------------------------------------------------------------------
// Aliasing
// ---------------------------------------------------------------------------
 
TEST(PushBack, SelfReferencingPushSurvivesReallocation) {
    // v.push_back(v[0]) when full: the growth frees the old buffer, and `value`
    // is a reference INTO that buffer. A naive implementation reads freed
    // memory here. ASan reports use-after-free; the value check catches it
    // even without sanitizers.
    vector<int> v;
    v.reserve(4);
    for (int i = 1; i <= 4; ++i) v.push_back(i);
    ASSERT_EQ(v.size(), v.capacity());
 
    v.push_back(v[0]);
 
    ASSERT_EQ(v.size(), 5u);
    EXPECT_EQ(v.back(), 1) << "push_back read from the buffer it had already freed";
}
 
TEST(PushBack, SelfReferencingMovePushSurvivesReallocation) {
    vector<std::string> v;
    v.reserve(2);
    v.push_back("first");
    v.push_back("second");
    ASSERT_EQ(v.size(), v.capacity());
 
    v.push_back(std::move(v[0]));
 
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[2], "first") << "the moved-from source lived in the freed buffer";
}
 
// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------
 
TEST(PushBack, ThrowingElementCopyLeavesTheVectorUnchanged) {
    // size_ is incremented last, so a throwing construction must leave the
    // vector exactly as it was.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(1);
        vector<ThrowOnCopy> v;
        v.reserve(10);
        for (int i = 0; i < 3; ++i) v.push_back(proto);
        ASSERT_EQ(v.size(), 3u);
 
        ThrowOnCopy::budget = 0;
        EXPECT_THROW({ v.push_back(proto); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 3u) << "size counted an element that was never constructed";
        EXPECT_EQ(v.back().value, 1) << "the existing elements were damaged";
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(PushBack, ThrowingGrowthLeavesTheVectorUnchanged) {
    // The throw happens inside reserve, during the transfer of existing
    // elements. reserve is strong, so nothing should change.
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(7);
        vector<ThrowOnCopy> v;
        v.reserve(4);
        for (int i = 0; i < 4; ++i) v.push_back(proto);
        const ThrowOnCopy* buffer = v.data();
        const auto cap = v.capacity();
 
        ThrowOnCopy::budget = 2;  // fails partway through transferring the 4
        EXPECT_THROW({ v.push_back(proto); }, std::runtime_error);
 
        EXPECT_EQ(v.size(), 4u);
        EXPECT_EQ(v.capacity(), cap);
        EXPECT_EQ(v.data(), buffer) << "the original buffer was released";
        EXPECT_EQ(v[3].value, 7);
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 
TEST(PushBack, VectorStillUsableAfterAFailedPush) {
    ThrowOnCopy::reset(/*budget=*/1000);
    {
        ThrowOnCopy proto(3);
        vector<ThrowOnCopy> v;
        v.reserve(10);
        v.push_back(proto);
 
        ThrowOnCopy::budget = 0;
        EXPECT_THROW({ v.push_back(proto); }, std::runtime_error);
 
        ThrowOnCopy::budget = 5;
        EXPECT_NO_THROW({ v.push_back(proto); });
        EXPECT_EQ(v.size(), 2u);
    }
    EXPECT_EQ(ThrowOnCopy::live, 0);
}
 