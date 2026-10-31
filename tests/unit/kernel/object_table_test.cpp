/**
 * Unit tests for kernel object system (XObject and ObjectTable)
 *
 * Tests handle management, reference counting, and name mapping.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>

using namespace rex;
using namespace rex::system;
using namespace rex::system::util;

// =============================================================================
// Test Fixtures and Helpers
// =============================================================================

// Minimal XObject subclass for testing
class TestObject : public XObject {
 public:
  static constexpr Type kObjectType = Type::Undefined;

  TestObject() : XObject(Type::Undefined) {}

  // Track destructor calls for leak detection
  static int destructor_count;
  ~TestObject() override { destructor_count++; }
};

int TestObject::destructor_count = 0;

// Reset destructor count before each test
struct DestructorCountReset {
  DestructorCountReset() { TestObject::destructor_count = 0; }
};

// Initialize logging once
void InitTestLogging() {
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    initialized = true;
  }
}

// =============================================================================
// object_ref Smart Pointer Tests
// =============================================================================

TEST_CASE("object_ref default construction is null", "[kernel][object_ref]") {
  InitTestLogging();
  object_ref<TestObject> ref;

  CHECK(ref.get() == nullptr);
  CHECK(!ref);
  CHECK(ref == nullptr);
}

TEST_CASE("object_ref construction from raw pointer", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();
  // Object starts with ref count of 1 from constructor

  {
    object_ref<TestObject> ref(obj);
    CHECK(ref.get() == obj);
    CHECK(ref);
    CHECK(ref != nullptr);
  }
  // Destructor should release, destroying object
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref copy construction retains", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    {
      object_ref<TestObject> ref2(ref1);  // Copy - should retain
      CHECK(ref1.get() == ref2.get());
      CHECK(TestObject::destructor_count == 0);  // Still alive
    }
    // ref2 destroyed, but ref1 still holds
    CHECK(TestObject::destructor_count == 0);
  }
  // Both refs destroyed
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref move construction transfers ownership", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2(std::move(ref1));

    CHECK(ref1.get() == nullptr);  // Moved from
    CHECK(ref2.get() == obj);
    CHECK(TestObject::destructor_count == 0);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref copy assignment retains", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2;

    ref2 = ref1;  // Copy assign
    CHECK(ref1.get() == ref2.get());
    CHECK(TestObject::destructor_count == 0);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref move assignment transfers ownership", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2;

    ref2 = std::move(ref1);
    CHECK(ref1.get() == nullptr);
    CHECK(ref2.get() == obj);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref reset releases reference", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  object_ref<TestObject> ref(obj);
  CHECK(TestObject::destructor_count == 0);

  ref.reset();
  CHECK(ref.get() == nullptr);
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref release returns pointer without destroying", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  object_ref<TestObject> ref(obj);
  auto* released = ref.release();

  CHECK(released == obj);
  CHECK(ref.get() == nullptr);
  CHECK(TestObject::destructor_count == 0);  // Not destroyed

  // Clean up manually
  released->Release();
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref arrow operator works", "[kernel][object_ref]") {
  InitTestLogging();

  auto* obj = new TestObject();
  object_ref<TestObject> ref(obj);

  CHECK(ref->type() == XObject::Type::Undefined);
}

TEST_CASE("object_ref dereference operator works", "[kernel][object_ref]") {
  InitTestLogging();

  auto* obj = new TestObject();
  object_ref<TestObject> ref(obj);

  TestObject& deref = *ref;
  CHECK(&deref == obj);
}

TEST_CASE("retain_object helper retains and wraps", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();
  // obj has ref count 1

  {
    auto ref = retain_object(obj);  // Should retain, ref count now 2
    CHECK(ref.get() == obj);
  }
  // ref destroyed, ref count back to 1
  CHECK(TestObject::destructor_count == 0);

  obj->Release();  // Final release
  CHECK(TestObject::destructor_count == 1);
}

// =============================================================================
// ObjectTable Handle Allocation Tests
// =============================================================================

TEST_CASE("ObjectTable AddHandle allocates valid handle", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  X_STATUS status = table.AddHandle(obj, &handle);

  CHECK(status == X_STATUS_SUCCESS);
  CHECK(handle != 0);
  CHECK(handle >= XObject::kHandleBase);  // 0xF8000000

  // Handle should be in object's handle list
  CHECK(obj->handles().size() == 1);
  CHECK(obj->handles()[0] == handle);

  table.Reset();
}

TEST_CASE("ObjectTable first handle is slot 1 not 0", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  // First handle should be slot 1: kHandleBase + (1 << 2) = 0xF8000004
  CHECK(handle == XObject::kHandleBase + 4);

  table.Reset();
}

TEST_CASE("ObjectTable multiple objects get unique handles", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();
  auto* obj3 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0, h3 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);
  table.AddHandle(obj3, &h3);

  CHECK(h1 != h2);
  CHECK(h2 != h3);
  CHECK(h1 != h3);

  // Handles increment by 4 (slot << 2)
  CHECK(h2 == h1 + 4);
  CHECK(h3 == h2 + 4);

  table.Reset();
}

// MoveToHighSlot (DOA4 netplay debug 82B70000): a lazily created object (GetNativeObject, key = its guest header
// address) moves into the lazy pool at a slot keyed by that address, and the low slot it frees goes to the next object
// added, so the handles of later objects depend neither on the lazy object nor on how many came before it.
namespace {
// The home slot, written out independently of ObjectTable::LazyHomeSlot: the top 13 bits of (key / 4) * 2654435761
// (mod 2^32), counted down from slot 16383 (the table's first size is 16 * 1024 slots).
uint32_t Home(uint32_t key) {
  return 16383u - static_cast<uint32_t>((static_cast<uint64_t>(key >> 2) * 2654435761u) & 0xFFFFFFFFu) / (1u << 19);
}
X_HANDLE SlotHandle(uint32_t slot) {
  return XObject::kHandleBase + (slot << 2);
}
TestObject* Added(ObjectTable& table) {
  auto* o = new TestObject();
  X_HANDLE h = 0;
  REQUIRE(table.AddHandle(o, &h) == X_STATUS_SUCCESS);
  return o;
}
}  // namespace

TEST_CASE("ObjectTable MoveToHighSlot frees the low slot for the next object", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* a = new TestObject();
  auto* b = new TestObject();
  auto* c = new TestObject();
  X_HANDLE ha = 0, hb = 0, hc = 0;
  table.AddHandle(a, &ha);
  table.AddHandle(b, &hb);
  table.AddHandle(c, &hc);
  REQUIRE(hb == ha + 4);
  REQUIRE(hc == hb + 4);

  const uint32_t kb = 0x82B747D0;
  REQUIRE(ObjectTable::LazyHomeSlot(kb) == Home(kb));
  REQUIRE(Home(kb) >= 8192);
  CHECK(table.MoveToHighSlot(b, kb) == X_STATUS_SUCCESS);
  REQUIRE(b->handles().size() == 1);
  CHECK(b->handles()[0] == SlotHandle(Home(kb)));
  CHECK(table.LookupObject<XObject>(SlotHandle(Home(kb))).get() == b);
  CHECK(table.LookupObject<XObject>(hb).get() == nullptr);

  // D takes B's old slot, not the slot after C.
  auto* d = new TestObject();
  X_HANDLE hd = 0;
  table.AddHandle(d, &hd);
  CHECK(hd == hb);
  CHECK(table.LookupObject<XObject>(hd).get() == d);

  table.Reset();
}

TEST_CASE("ObjectTable MoveToHighSlot places by key, not by creation order", "[kernel][object_table]") {
  InitTestLogging();

  // Three real DOA4 headers with distinct home slots.
  const uint32_t kx = 0x82B7477C, ky = 0x82B7476C, kz = 0x82B747D0;
  REQUIRE(Home(kx) != Home(ky));
  REQUIRE(Home(kx) != Home(kz));
  REQUIRE(Home(ky) != Home(kz));

  // Peer 1: X then Y. Peer 2: an extra lazy object Z first (host-timed), then X and Y.
  ObjectTable t1, t2;
  auto* x1 = Added(t1);
  auto* y1 = Added(t1);
  REQUIRE(t1.MoveToHighSlot(x1, kx) == X_STATUS_SUCCESS);
  REQUIRE(t1.MoveToHighSlot(y1, ky) == X_STATUS_SUCCESS);
  auto* z2 = Added(t2);
  auto* x2 = Added(t2);
  auto* y2 = Added(t2);
  REQUIRE(t2.MoveToHighSlot(z2, kz) == X_STATUS_SUCCESS);
  REQUIRE(t2.MoveToHighSlot(x2, kx) == X_STATUS_SUCCESS);
  REQUIRE(t2.MoveToHighSlot(y2, ky) == X_STATUS_SUCCESS);
  CHECK(x1->handles()[0] == x2->handles()[0]);
  CHECK(y1->handles()[0] == y2->handles()[0]);
  CHECK(x1->handles()[0] == SlotHandle(Home(kx)));
  CHECK(y1->handles()[0] == SlotHandle(Home(ky)));

  // The next ordinary object gets the same handle on both peers.
  auto* n1 = new TestObject();
  auto* n2 = new TestObject();
  X_HANDLE hn1 = 0, hn2 = 0;
  t1.AddHandle(n1, &hn1);
  t2.AddHandle(n2, &hn2);
  CHECK(hn1 == hn2);

  t1.Reset();
  t2.Reset();
}

TEST_CASE("ObjectTable MoveToHighSlot probes down from a taken home slot", "[kernel][object_table]") {
  InitTestLogging();

  // Two keys with the same home slot (the residual case: whichever comes first takes the home).
  const uint32_t k1 = 0x40000000;
  uint32_t k2 = k1 + 4;
  while (Home(k2) != Home(k1)) {
    k2 += 4;
  }
  ObjectTable table;
  auto* p = Added(table);
  auto* q = Added(table);
  REQUIRE(table.MoveToHighSlot(p, k1) == X_STATUS_SUCCESS);
  REQUIRE(table.MoveToHighSlot(q, k2) == X_STATUS_SUCCESS);
  CHECK(p->handles()[0] == SlotHandle(Home(k1)));
  const uint32_t below = Home(k1) == 8192 ? 16383 : Home(k1) - 1;  // wraps from the pool's base to its top
  CHECK(q->handles()[0] == SlotHandle(below));

  // The probe wraps: a key whose home is the pool's base, with the base taken, goes to the top slot.
  uint32_t kb = 0x50000000;
  while (Home(kb) != 8192) {
    kb += 4;
  }
  uint32_t kb2 = kb + 4;
  while (Home(kb2) != 8192) {
    kb2 += 4;
  }
  auto* r = Added(table);
  auto* s = Added(table);
  REQUIRE(table.MoveToHighSlot(r, kb) == X_STATUS_SUCCESS);
  REQUIRE(table.MoveToHighSlot(s, kb2) == X_STATUS_SUCCESS);
  CHECK(r->handles()[0] == SlotHandle(8192));
  CHECK(s->handles()[0] == SlotHandle(16383));

  table.Reset();
}

TEST_CASE("ObjectTable MoveToHighSlot gives a re-initialised header's slot to its new object",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  const uint32_t k = 0x401F5240;  // a guest heap event DOA4 re-initialises dozens of times per session
  auto* first = Added(table);
  first->Retain();  // keep it alive to look at it after the table lets go
  REQUIRE(table.MoveToHighSlot(first, k) == X_STATUS_SUCCESS);
  const X_HANDLE home = SlotHandle(Home(k));
  REQUIRE(first->handles()[0] == home);

  auto* again = Added(table);
  REQUIRE(table.MoveToHighSlot(again, k) == X_STATUS_SUCCESS);
  CHECK(again->handles()[0] == home);
  CHECK(table.LookupObject<XObject>(home).get() == again);
  CHECK(first->handles().empty());
  // An evicted object has no handle: handle() is 0 and ReleaseHandle is a no-op (no handles_[0] read; this
  // TestObject has no kernel state, so a read through it would crash).
  CHECK(first->handle() == 0);
  CHECK_FALSE(first->ReleaseHandle());
  CHECK(table.LookupObject<XObject>(home).get() == again);

  // Once its slot was released and taken by another key, the old key's record no longer frees anything.
  table.ReleaseHandle(home);  // the slot is free again
  uint32_t other = k + 4;
  while (Home(other) != Home(k)) {
    other += 4;
  }
  auto* intruder = Added(table);
  REQUIRE(table.MoveToHighSlot(intruder, other) == X_STATUS_SUCCESS);
  REQUIRE(intruder->handles()[0] == home);
  auto* third = Added(table);
  REQUIRE(table.MoveToHighSlot(third, k) == X_STATUS_SUCCESS);
  CHECK(table.LookupObject<XObject>(home).get() == intruder);
  CHECK(third->handles()[0] != home);

  first->Release();
  table.Reset();
}

TEST_CASE("ObjectTable MoveToHighSlot never evicts an ordinary object from a released lazy slot",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  const uint32_t k = 0x401F5240;
  auto* lazy = Added(table);
  REQUIRE(table.MoveToHighSlot(lazy, k) == X_STATUS_SUCCESS);
  const X_HANDLE home = SlotHandle(Home(k));
  REQUIRE(lazy->handles()[0] == home);

  // The lazy object's handle is released (ObDereferenceObject -> ReleaseHandle): its slot is free.
  REQUIRE(table.ReleaseHandle(home) == X_STATUS_SUCCESS);
  REQUIRE(table.LookupObject<XObject>(home).get() == nullptr);

  // Ordinary objects fill the table from the low end until one takes that slot.
  TestObject* ordinary = nullptr;
  for (uint32_t i = 0; i < 16384 && !ordinary; ++i) {
    auto* o = new TestObject();
    X_HANDLE h = 0;
    REQUIRE(table.AddHandle(o, &h) == X_STATUS_SUCCESS);
    if (h == home) {
      ordinary = o;
    }
  }
  REQUIRE(ordinary != nullptr);

  // The guest re-initialises the same header: the ordinary object keeps its handle and its table entry.
  auto* again = Added(table);
  REQUIRE(table.MoveToHighSlot(again, k) == X_STATUS_SUCCESS);
  CHECK(table.LookupObject<XObject>(home).get() == ordinary);
  REQUIRE(ordinary->handles().size() == 1);
  CHECK(ordinary->handles()[0] == home);
  CHECK(again->handles()[0] != home);

  table.Reset();
}

TEST_CASE("ObjectTable MoveToHighSlot fails when the pool is full or the object is not held",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  // Fill all 8192 pool slots with distinct keys.
  for (uint32_t i = 0; i < 8192; ++i) {
    auto* o = Added(table);
    REQUIRE(table.MoveToHighSlot(o, 0x30000000 + i * 4) == X_STATUS_SUCCESS);
  }
  auto* late = Added(table);
  const X_HANDLE low = late->handles()[0];
  CHECK(table.MoveToHighSlot(late, 0x3FFFFFF0) == X_STATUS_UNSUCCESSFUL);
  CHECK(late->handles()[0] == low);
  CHECK(table.LookupObject<XObject>(low).get() == late);

  // An object with no handle in this table.
  auto* loose = new TestObject();
  CHECK(table.MoveToHighSlot(loose, 0x30000000) == X_STATUS_INVALID_HANDLE);
  CHECK(loose->handles().empty());
  loose->Release();

  table.Reset();
}

// =============================================================================
// ObjectTable Handle Lookup Tests
// =============================================================================

TEST_CASE("ObjectTable LookupObject finds object by handle", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == obj);

  table.Reset();
}

TEST_CASE("ObjectTable LookupObject returns null for invalid handle", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  auto found = table.LookupObject<XObject>(0xF8001234);
  CHECK(found.get() == nullptr);
}

TEST_CASE("ObjectTable LookupObject returns null for handle 0", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  auto found = table.LookupObject<XObject>(0);
  CHECK(found.get() == nullptr);
}

// =============================================================================
// ObjectTable Handle Reference Counting Tests
// =============================================================================

TEST_CASE("ObjectTable RetainHandle increments ref count", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.RetainHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  // Need two releases to remove
  table.ReleaseHandle(handle);
  auto still_there = table.LookupObject<XObject>(handle);
  CHECK(still_there.get() == obj);

  table.ReleaseHandle(handle);  // This should remove it

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle removes at zero refs", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.ReleaseHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  // Handle should be gone
  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == nullptr);

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle on invalid handle returns error", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  X_STATUS status = table.ReleaseHandle(0xF8001234);
  CHECK(status == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable DuplicateHandle creates new handle for same object",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj, &h1);
  table.DuplicateHandle(h1, &h2);

  CHECK(h1 != h2);

  // Both handles refer to same object
  auto found1 = table.LookupObject<XObject>(h1);
  auto found2 = table.LookupObject<XObject>(h2);
  CHECK(found1.get() == obj);
  CHECK(found2.get() == obj);

  // Object should have both handles
  CHECK(obj->handles().size() == 2);

  table.Reset();
}

// =============================================================================
// ObjectTable Handle Release Tests
// =============================================================================

TEST_CASE("ObjectTable ReleaseHandle removes handle from table", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  // ReleaseHandle decrements ref count; at 0 it removes the handle
  X_STATUS status = table.ReleaseHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == nullptr);

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle updates object's handle list", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();
  obj->Retain();  // Keep alive after table releases

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);
  CHECK(obj->handles().size() == 1);

  table.ReleaseHandle(handle);
  CHECK(obj->handles().empty());

  obj->Release();
  table.Reset();
}

// =============================================================================
// ObjectTable Name Mapping Tests
// =============================================================================

TEST_CASE("ObjectTable AddNameMapping registers name", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.AddNameMapping("TestName", handle);
  CHECK(status == X_STATUS_SUCCESS);

  table.Reset();
}

TEST_CASE("ObjectTable duplicate name returns collision", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);

  CHECK(table.AddNameMapping("SharedName", h1) == X_STATUS_SUCCESS);
  CHECK(table.AddNameMapping("SharedName", h2) == X_STATUS_OBJECT_NAME_COLLISION);

  table.Reset();
}

TEST_CASE("ObjectTable name collision is case-insensitive", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);

  CHECK(table.AddNameMapping("TestName", h1) == X_STATUS_SUCCESS);
  // Different case should still collide
  CHECK(table.AddNameMapping("testname", h2) == X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(table.AddNameMapping("TESTNAME", h2) == X_STATUS_OBJECT_NAME_COLLISION);

  table.Reset();
}

TEST_CASE("ObjectTable GetObjectByName returns not found for missing name",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  X_HANDLE found = 0;
  X_STATUS status = table.GetObjectByName("DoesNotExist", &found);
  CHECK(status == X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(found == X_INVALID_HANDLE_VALUE);
}

TEST_CASE("ObjectTable RemoveNameMapping clears name", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);
  table.AddNameMapping("TempName", handle);

  table.RemoveNameMapping("TempName");

  X_HANDLE found = 0;
  CHECK(table.GetObjectByName("TempName", &found) == X_STATUS_OBJECT_NAME_NOT_FOUND);

  table.Reset();
}

// =============================================================================
// ObjectTable Reset and Bulk Operations
// =============================================================================

TEST_CASE("ObjectTable Reset releases all objects", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();
  auto* obj3 = new TestObject();

  table.AddHandle(obj1, nullptr);
  table.AddHandle(obj2, nullptr);
  table.AddHandle(obj3, nullptr);

  // Objects have ref count 2: 1 from new, 1 from AddHandle's Retain
  // Release our initial ref so table owns them exclusively
  obj1->Release();
  obj2->Release();
  obj3->Release();

  CHECK(TestObject::destructor_count == 0);

  table.Reset();

  // Now Reset's Release brings ref count from 1 to 0, destroying them
  CHECK(TestObject::destructor_count == 3);
}

TEST_CASE("ObjectTable GetAllObjects returns all objects", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  table.AddHandle(obj1, nullptr);
  table.AddHandle(obj2, nullptr);

  auto all = table.GetAllObjects();
  CHECK(all.size() == 2);

  table.Reset();
}

// =============================================================================
// TODO: Tests requiring kernel integration
// =============================================================================

// TODO: Test handle 0xFFFFFFFF (CurrentProcess) - requires full KernelState
// TODO: Test handle 0xFFFFFFFE (CurrentThread) - requires XThread integration
// TODO: Test GetObjectsByType<T>() with real typed objects
// TODO: Test GetObjectByName with existing object - requires kernel_state for RetainHandle
// TODO: Test case-insensitive name lookup via GetObjectByName
