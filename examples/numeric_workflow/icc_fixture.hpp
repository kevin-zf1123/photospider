#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Shared structural ICC fixture for the manual public-API examples. This does
// not represent a printing condition or provide a color transform oracle.
namespace numeric_fixture {
inline void word(std::vector<std::uint8_t>* bytes, std::size_t at,
                 std::uint32_t value, unsigned size = 4) {
  for (unsigned i = 0; i < size; ++i)
    bytes->at(at + i) =
        static_cast<std::uint8_t>(value >> (8 * (size - i - 1)));
}
inline void key(std::vector<std::uint8_t>* bytes, std::size_t at,
                const char* text) {
  std::memcpy(bytes->data() + at, text, 4);
}
inline std::vector<std::uint8_t> table(unsigned inputs, unsigned outputs) {
  const auto count =
      52 + 2 * (inputs * 2 + (1U << inputs) * outputs + outputs * 2);
  std::vector<std::uint8_t> bytes(count);
  key(&bytes, 0, "mft2");
  bytes[8] = inputs;
  bytes[9] = outputs;
  bytes[10] = 2;
  for (unsigned i = 0; i < 9; i += 4)
    word(&bytes, 12 + 4 * i, 65536);
  word(&bytes, 48, 2, 2);
  word(&bytes, 50, 2, 2);
  // Synthetic structural fixture; its zero tables do not model a printer.
  return bytes;
}
inline std::vector<std::uint8_t> fixture(
    const std::vector<std::uint8_t>& localized = {}) {
  std::vector<std::uint8_t> bytes(252);
  word(&bytes, 8, 0x04400000);
  key(&bytes, 12, "prtr");
  key(&bytes, 16, "CMYK");
  key(&bytes, 20, "Lab ");
  key(&bytes, 36, "acsp");
  word(&bytes, 24, 2026, 2);
  word(&bytes, 26, 9, 2);
  word(&bytes, 28, 20, 2);
  word(&bytes, 68, 0xf6d6);
  word(&bytes, 72, 65536);
  word(&bytes, 76, 0xd32d);
  word(&bytes, 128, 10);
  std::size_t index = 0;
  const auto append = [&](const char* name,
                          const std::vector<std::uint8_t>& payload) {
    key(&bytes, 132 + 12 * index, name);
    word(&bytes, 136 + 12 * index, bytes.size());
    word(&bytes, 140 + 12 * index, payload.size());
    ++index;
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    while (bytes.size() % 4)
      bytes.push_back(0);
  };
  const auto repeat = [&](const char* name, std::size_t source) {
    key(&bytes, 132 + 12 * index, name);
    std::memcpy(bytes.data() + 136 + 12 * index,
                bytes.data() + 136 + 12 * source, 8);
    ++index;
  };
  std::vector<std::uint8_t> text(30);
  key(&text, 0, "mluc");
  word(&text, 8, 1);
  word(&text, 12, 12);
  text[16] = 'e';
  text[17] = 'n';
  text[18] = 'U';
  text[19] = 'S';
  word(&text, 20, 2);
  word(&text, 24, 28);
  text[29] = 'X';
  if (!localized.empty())
    text = localized;
  append("desc", text);
  repeat("cprt", 0);
  std::vector<std::uint8_t> white(20);
  key(&white, 0, "XYZ ");
  word(&white, 8, 0xf6d6);
  word(&white, 12, 65536);
  word(&white, 16, 0xd32d);
  append("wtpt", white);
  append("A2B0", table(4, 3));
  repeat("A2B1", 3);
  repeat("A2B2", 3);
  append("B2A0", table(3, 4));
  repeat("B2A1", 6);
  repeat("B2A2", 6);
  append("gamt", table(3, 1));
  word(&bytes, 0, bytes.size());
  return bytes;
}
}  // namespace numeric_fixture
