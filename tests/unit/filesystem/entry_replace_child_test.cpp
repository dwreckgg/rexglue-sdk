/**
 * @file        tests/unit/filesystem/entry_replace_child_test.cpp
 * @brief       Unit tests for Entry::ReplaceChild
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>

namespace {

using rex::filesystem::Device;
using rex::filesystem::Entry;
using rex::X_STATUS;  // the X_STATUS_* macros spell the type unqualified

class TestDevice final : public Device {
 public:
  TestDevice() : Device("\\Device\\Test") {}
  bool Initialize() override { return true; }
  void Dump(rex::string::StringBuffer*) override {}
  Entry* ResolvePath(const std::string_view) override { return nullptr; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 0; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 2048; }

 private:
  std::string name_ = "Test";
};

class TestEntry final : public Entry {
 public:
  TestEntry(Device* device, Entry* parent, std::string_view path, int tag)
      : Entry(device, parent, path), tag(tag) {}
  X_STATUS Open(uint32_t, rex::filesystem::File**) override { return X_STATUS_NOT_IMPLEMENTED; }
  void Add(std::unique_ptr<Entry> child) { children_.push_back(std::move(child)); }
  const int tag;
};

int Tag(const Entry* entry) { return entry ? static_cast<const TestEntry*>(entry)->tag : -1; }

}  // namespace

TEST_CASE("Entry::ReplaceChild swaps the same-named child in place", "[filesystem]") {
  TestDevice device;
  TestEntry root(&device, nullptr, "", 0);
  root.Add(std::make_unique<TestEntry>(&device, &root, "a.afs", 1));
  root.Add(std::make_unique<TestEntry>(&device, &root, "chrone.afs", 2));
  root.Add(std::make_unique<TestEntry>(&device, &root, "z.afs", 3));

  std::unique_ptr<Entry> old =
      root.ReplaceChild(std::make_unique<TestEntry>(&device, &root, "CHRONE.AFS", 4));

  CHECK(Tag(old.get()) == 2);
  CHECK(root.child_count() == 3);
  CHECK(Tag(root.GetChild("chrone.afs")) == 4);
  CHECK(Tag(root.children()[1].get()) == 4);  // same position: enumeration order kept
  CHECK(Tag(root.GetChild("a.afs")) == 1);
  CHECK(Tag(root.GetChild("z.afs")) == 3);
}

TEST_CASE("Entry::ReplaceChild adds a child that has no namesake", "[filesystem]") {
  TestDevice device;
  TestEntry root(&device, nullptr, "", 0);
  root.Add(std::make_unique<TestEntry>(&device, &root, "a.afs", 1));

  std::unique_ptr<Entry> old =
      root.ReplaceChild(std::make_unique<TestEntry>(&device, &root, "new.afs", 5));

  CHECK(old == nullptr);
  CHECK(root.child_count() == 2);
  CHECK(Tag(root.GetChild("new.afs")) == 5);
  CHECK(Tag(root.children().back().get()) == 5);  // appended: enumeration order of the rest kept
  CHECK(Tag(root.GetChild("a.afs")) == 1);
}
