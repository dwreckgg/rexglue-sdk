/**
 * Unit tests for Entry::set_fixed_timestamp (netplay: every VFS entry reports one fixed time).
 *
 * Two installs of the same game folder differ only in their host file times; a lockstep session
 * must show the guest the same times on both PCs. With the switch on, every time an entry reports
 * is the fixed value; with it off (the default), the host file's real times.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace rex::filesystem;

namespace {

// 2009-01-01 00:00:00 UTC as a FILETIME; the host's own times below are 2020 and 2021, so the
// fixed value is unlike all three of them.
constexpr uint64_t kFixed = 0x01C96BA3E3A74000ull;
// The host times the test writes: creation, last access, last write (all distinct).
constexpr uint64_t kCreate = 0x01D5C03669050000ull;  // 2020-01-01
constexpr uint64_t kAccess = 0x01D6DFD10C358000ull;  // 2021-01-01
constexpr uint64_t kWrite = 0x01D6E09A369F4000ull;   // 2021-01-02

FILETIME Ft(uint64_t v) {
  FILETIME f;
  f.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFF);
  f.dwHighDateTime = static_cast<DWORD>(v >> 32);
  return f;
}

// A temporary folder holding data.bin (with the three host times set) and sub\more.bin; removed on exit.
struct HostFolder {
  std::filesystem::path root;
  HostFolder() {
    root = std::filesystem::temp_directory_path() /
           ("rex_fixed_ts_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root / "sub");
    { std::ofstream(root / "data.bin", std::ios::binary) << "0123456789"; }
    { std::ofstream(root / "sub" / "more.bin", std::ios::binary) << "x"; }
    for (const auto& p : {root / "data.bin", root / "sub" / "more.bin"}) {
      HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      REQUIRE(h != INVALID_HANDLE_VALUE);
      const FILETIME c = Ft(kCreate), a = Ft(kAccess), w = Ft(kWrite);
      REQUIRE(SetFileTime(h, &c, &a, &w));
      CloseHandle(h);
    }
  }
  ~HostFolder() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

// Restores the process-wide switch whatever a test leaves behind.
struct SwitchOff {
  ~SwitchOff() { Entry::set_fixed_timestamp(0); }
};

}  // namespace

TEST_CASE("fixed timestamp: off by default, the host file's real times", "[fixed_timestamp]") {
  SwitchOff restore;
  REQUIRE(Entry::fixed_timestamp() == 0);
  HostFolder folder;
  HostPathDevice device("\\Device\\Test", folder.root, true);
  REQUIRE(device.Initialize());
  Entry* e = device.ResolvePath("data.bin");
  REQUIRE(e != nullptr);
  CHECK(e->create_timestamp() == kCreate);
  CHECK(e->access_timestamp() == kAccess);
  CHECK(e->write_timestamp() == kWrite);
  CHECK(e->size() == 10);
}

TEST_CASE("fixed timestamp: on, every time of every entry is the fixed value", "[fixed_timestamp]") {
  SwitchOff restore;
  HostFolder folder;
  // Populated before the switch is set (the game folder is mounted before a session starts): the
  // switch is read at every query, not copied into an entry when it is created.
  HostPathDevice device("\\Device\\Test", folder.root, true);
  REQUIRE(device.Initialize());
  Entry::set_fixed_timestamp(kFixed);
  CHECK(Entry::fixed_timestamp() == kFixed);
  for (const char* path : {"data.bin", "sub", "sub\\more.bin"}) {
    INFO(path);
    Entry* e = device.ResolvePath(path);
    REQUIRE(e != nullptr);
    CHECK(e->create_timestamp() == kFixed);
    CHECK(e->access_timestamp() == kFixed);
    CHECK(e->write_timestamp() == kFixed);
  }
  // only the times: the size is still the file's
  CHECK(device.ResolvePath("data.bin")->size() == 10);
  // an entry created while the switch is on reports the fixed value too
  HostPathDevice device2("\\Device\\Test2", folder.root, true);
  REQUIRE(device2.Initialize());
  CHECK(device2.ResolvePath("data.bin")->create_timestamp() == kFixed);
}

TEST_CASE("fixed timestamp: switched off again, the real times come back", "[fixed_timestamp]") {
  SwitchOff restore;
  HostFolder folder;
  HostPathDevice device("\\Device\\Test", folder.root, true);
  REQUIRE(device.Initialize());
  Entry::set_fixed_timestamp(kFixed);
  Entry* e = device.ResolvePath("data.bin");
  REQUIRE(e != nullptr);
  REQUIRE(e->write_timestamp() == kFixed);
  Entry::set_fixed_timestamp(0);
  CHECK(Entry::fixed_timestamp() == 0);
  CHECK(e->create_timestamp() == kCreate);
  CHECK(e->access_timestamp() == kAccess);
  CHECK(e->write_timestamp() == kWrite);
}
