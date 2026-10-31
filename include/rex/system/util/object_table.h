#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>
#include <unordered_map>
#include <vector>

#include <rex/string/key.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::system::util {

class ObjectTable {
 public:
  ObjectTable();
  ~ObjectTable();

  void Reset();

  X_STATUS AddHandle(XObject* object, X_HANDLE* out_handle);
  X_STATUS DuplicateHandle(X_HANDLE orig, X_HANDLE* out_handle);
  X_STATUS RetainHandle(X_HANDLE handle);
  X_STATUS ReleaseHandle(X_HANDLE handle);
  X_STATUS RemoveHandle(X_HANDLE handle);
  // Moves the object's only handle (the one its constructor just added) into the lazy pool, slots kLazyPoolBase ..
  // kLazyPoolTop, at the first free slot probing downward (wrapping) from LazyHomeSlot(key). For the host objects
  // GetNativeObject creates on a guest dispatcher header's first use; key = the header's guest address. That use can
  // come at a host-timed moment (a wait on a contended critical section), and a slot taken from the low end there
  // would shift every handle the guest is given afterwards: moved out, the freed low slot goes to the next object
  // created, so the handles the guest sees follow only the guest's own creations. The slot depends on the key, not on
  // how many lazy objects came before, so a host-timed one on one peer does not move the others (residual: two keys
  // whose probe paths meet take each other's slot when their creation order differs). A key seen before (the guest
  // re-initialised that header, overwriting the stash) gives up its previous object's slot first: that object can no
  // longer be reached from guest memory and leaves the table, so re-initialised headers do not fill the pool.
  // DOA4 netplay debug 82B70000: no handle in this pool is ever handed to the guest (the game imports no
  // ObOpenObjectByPointer, the only export that returns a lazily created object's handle), so a slot difference
  // between peers shows only in stash words, which the .data sweep zeroes.
  // Returns X_STATUS_INVALID_HANDLE when the object does not hold exactly one handle in this table,
  // X_STATUS_UNSUCCESSFUL when the pool has no free slot; in both cases the handle stays where it is (the caller logs a
  // WARN).
  X_STATUS MoveToHighSlot(XObject* object, uint32_t key);
  static constexpr uint32_t kLazyPoolSize = 8192;
  static constexpr uint32_t kLazyPoolTop = 16383;  // the table's first size is 16 * 1024 slots
  static constexpr uint32_t kLazyPoolBase = kLazyPoolTop + 1 - kLazyPoolSize;
  static uint32_t LazyHomeSlot(uint32_t key);

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

  // Restores a XObject reference with a handle. Mainly for internal use - do
  // not use.
  X_STATUS RestoreHandle(X_HANDLE handle, XObject* object);

  template <typename T>
  object_ref<T> LookupObject(X_HANDLE handle) {
    auto object = LookupObject(handle, false);
    if (object) {
      if (object->type() != T::kObjectType) {
        object->Release();
        return object_ref<T>();
      }
    }
    auto result = object_ref<T>(reinterpret_cast<T*>(object));
    return result;
  }

  X_STATUS AddNameMapping(const std::string_view name, X_HANDLE handle);
  void RemoveNameMapping(const std::string_view name);
  X_STATUS GetObjectByName(const std::string_view name, X_HANDLE* out_handle);
  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType(XObject::Type type) {
    std::vector<object_ref<T>> results;
    GetObjectsByType(type, reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType() {
    std::vector<object_ref<T>> results;
    GetObjectsByType(T::kObjectType, reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  std::vector<object_ref<XObject>> GetAllObjects();
  void PurgeAllObjects();  // Purges the object table of all guest objects

 private:
  struct ObjectTableEntry {
    int handle_ref_count = 0;
    XObject* object = nullptr;
  };

  ObjectTableEntry* LookupTable(X_HANDLE handle);
  XObject* LookupObject(X_HANDLE handle, bool already_locked);
  void GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results);

  X_HANDLE TranslateHandle(X_HANDLE handle);
  static constexpr uint32_t GetHandleSlot(X_HANDLE handle) {
    return (handle - XObject::kHandleBase) >> 2;
  }
  X_STATUS FindFreeSlot(uint32_t* out_slot);
  bool Resize(uint32_t new_capacity);

  rex::thread::global_critical_region global_critical_region_;
  std::mutex name_mutex_;
  uint32_t table_capacity_ = 0;
  ObjectTableEntry* table_ = nullptr;
  uint32_t last_free_entry_ = 0;
  std::unordered_map<string::string_key_case, X_HANDLE> name_table_;
  // MoveToHighSlot's bookkeeping: the pool slot each key's latest object took, and the key that placed each slot.
  std::unordered_map<uint32_t, uint32_t> lazy_slot_by_key_;
  std::unordered_map<uint32_t, uint32_t> lazy_key_by_slot_;
};

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle);

}  // namespace rex::system::util
