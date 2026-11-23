// Links: tests/rt/kv_jsc_sparse_test.cpp src/kv_conversion/typed_value.cpp
#include "uos/kv_conversion/typed_value.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include <cstdlib>
#include <new>

namespace {
  // Bound all requested heap bytes during the operation, which also bounds its
  // peak live allocations. Fail before an allocation could exhaust the runner.
  constexpr std::size_t allocationLimit = 2 * 1024 * 1024;
  thread_local std::size_t* requested = nullptr;

  void* allocate(std::size_t size) {
    if (requested) {
      if (size > allocationLimit - *requested) throw std::bad_alloc();
      *requested += size;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
  }

  struct CountAllocations {
    std::size_t bytes = 0;
    CountAllocations() { requested = &bytes; }
    ~CountAllocations() { requested = nullptr; }
  };
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
  using namespace uos;
  using namespace uos::rt;

  struct SparseVector {
    const char* name;
    const char* hex;
    std::size_t length;
    std::vector<std::pair<std::size_t, KvValue>> entries;
  };

  // Bun 1.4.0 node:v8 deserialize, observing length and Object.keys, and reading
  // each own property. No operation on the oracle iterates over the holes.
  const SparseVector vectors[] = {
    {"original", "05000000012cfbff00ffffffff", 16775980, {}},
    {"1 Mi slots", "0e0000000100001000ffffffff", 1048576, {}},
    {"4 Mi slots", "0e0000000100004000ffffffff", 4194304, {}},
    {"old cap minus one", "0e00000001ffffff000000000003ffffffff", 16777215, {{0, KvValue()}}},
    {"old cap", "0e00000001000000010000010004ffffffff", 16777216, {{65536, nullptr}}},
    {"old cap plus one", "0e00000001010000010000000107ffffffff", 16777217, {{16777216, 1}}},
    {"8 Mi with first and last", "0e00000001000080000000000007ffff7f0008ffffffff", 8388608, {{0, 1}, {8388607, false}}},
    {"16 Mi reverse order", "0e000000010000000180969800070100000009ffffffff", 16777216, {{1, true}, {10000000, 1}}},
    {"16 Mi duplicate index", "0e000000010000000180969800078096980008ffffffff", 16777216, {{10000000, false}}},
    {"index grows length", "0e000000010b00000080a8120106ffffffff", 18000001, {{18000000, 0}}},
    {"2^31 length", "0e000000010000008080f0fa0203ffffffff", 2147483648, {{50000000, KvValue()}}},
    {"max length", "0e00000001ffffffffffffffff", 4294967295, {}},
    {"last possible index", "0e00000001fffffffffeffffff07ffffffff", 4294967295, {{4294967294, 1}}},
    {"numeric property grows length", "0e0000000104000000fdffffff0a0000803432393439363732393307ffffffff", 4294967294, {{4294967293, 1}}},
    {"two indices around the marker", "0e00000001fffffffffeffffff08fdffffff0a0000803432393439363732393307ffffffff", 4294967295, {{4294967293, 1}, {4294967294, false}}},
  };
}

TEST("Bun's large sparse arrays decode and transcode within a fixed allocation budget") {
  using namespace uos::kv_conversion::typed_value;
  for (const SparseVector& vector : vectors) {
    const KvBytes input = fromHex(vector.hex);
    KvValue result;
    KvValue roundTrip;
    std::size_t allocated = 0;
    {
      const CountAllocations count;
      result = kvJscDeserialize(input);
      const KvValue decoded = decodeTypedValue(encodeTypedValue(result));
      roundTrip = kvJscDeserialize(kvJscSerialize(decoded));
      allocated = count.bytes;
    }
    const KvValue& value = result;
    ASSERT(value.isArray());
    const KvArray& array = value.asArray();
    EXPECT_EQ(array.size(), vector.length);
    std::size_t index = array.nextPresent(0);
    for (const auto& [expected, member] : vector.entries) {
      EXPECT_EQ(index, expected);
      EXPECT(array[index] == member);
      index = array.nextPresent(index + 1);
    }
    EXPECT_EQ(index, array.size());
    EXPECT(value[2].isUndefined());
    EXPECT(array[2].is<KvHole>());
    EXPECT(roundTrip == result);
    EXPECT(allocated < allocationLimit);
  }
}

TEST("KvArray preserves mutable iteration for dense and sparse arrays") {
  KvArray dense{KvHole{}, true};
  for (KvValue& value : dense)
    if (value.is<KvHole>()) value = KvValue();
  EXPECT(dense[0].isUndefined());
  EXPECT(dense[1].asBool());

  KvArray sparse(16777217, KvHole{});
  std::size_t allocated = 0;
  {
    const CountAllocations count;
    *sparse.begin() = true;
    sparse.begin()[16777216] = KvValue();
    allocated = count.bytes;
  }
  const KvArray& read = sparse;
  EXPECT_EQ(read.end() - sparse.begin(), std::ptrdiff_t{16777217});
  EXPECT_EQ(read.nextPresent(0), std::size_t{0});
  EXPECT_EQ(read.nextPresent(1), std::size_t{16777216});
  EXPECT(read[0].asBool());
  EXPECT(read[1].is<KvHole>());
  EXPECT(read[16777216].isUndefined());
  EXPECT(allocated < allocationLimit);
}
