#include <spdlog/spdlog.h>
#include <unicorn/unicorn.h>


#include "synthetic_dll.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static bool envFlagEnabled(const char *name) {
  char *rawValue = nullptr;
  size_t valueSize = 0;
  if (_dupenv_s(&rawValue, &valueSize, name) != 0 || !rawValue)
    return false;
  std::string value(rawValue);
  std::free(rawValue);
  if (value.empty())
    return false;
  std::string text;
  for (char ch : value)
    text.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
  return text != "0" && text != "false" && text != "no" && text != "off";
}

static std::string narrowWideLossy(const std::wstring &value) {
  std::string narrowValue;
  narrowValue.reserve(value.size());
  for (wchar_t ch : value)
    narrowValue.push_back(ch >= 0 && ch <= 0x7f ? char(ch) : '?');
  return narrowValue;
}

static void writeGuestCommandLine(uc_engine *uc, uint32_t address,
                                  const std::wstring &value) {
  std::vector<uint16_t> units;
  units.reserve(value.size() + 1);
  for (wchar_t ch : value)
    units.push_back(static_cast<uint16_t>(ch));
  units.push_back(0);
  uc_mem_write(uc, address, units.data(), units.size() * sizeof(uint16_t));
}

static bool readGuestU16(uc_engine *uc, uint32_t address, uint16_t &value) {
  return address && uc_mem_read(uc, address, &value, sizeof(value)) == UC_ERR_OK;
}

static bool readGuestU32(uc_engine *uc, uint32_t address, uint32_t &value) {
  return address && uc_mem_read(uc, address, &value, sizeof(value)) == UC_ERR_OK;
}

static std::string readGuestHex(uc_engine *uc, uint32_t address, size_t size) {
  if (!address || !size) return {};
  std::vector<uint8_t> bytes(size);
  if (uc_mem_read(uc, address, bytes.data(), bytes.size()) != UC_ERR_OK)
    return {};
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i) out << ' ';
    out << std::setw(2) << unsigned(bytes[i]);
  }
  return out.str();
}

static std::string readGuestWideLossy(uc_engine *uc, uint32_t address,
                                      size_t maxChars = 260) {
  std::string out;
  if (!address) return out;
  out.reserve(maxChars);
  for (size_t i = 0; i < maxChars; ++i) {
    uint16_t ch = 0;
    if (uc_mem_read(uc, address + uint32_t(i * 2), &ch, sizeof(ch)) != UC_ERR_OK || !ch)
      break;
    out.push_back(ch <= 0x7f ? char(ch) : '?');
  }
  return out;
}

struct ProfileDiagState {
  std::string currentConfigPath;
  uint32_t currentConfigKind{};
};

static void configureLogging() {
  char *rawValue = nullptr;
  size_t valueSize = 0;
  if (_dupenv_s(&rawValue, &valueSize, "INAVI_EMU_LOG") != 0 || !rawValue ||
      !*rawValue) {
    if (rawValue)
      std::free(rawValue);
#if defined(NDEBUG)
    spdlog::set_level(spdlog::level::err);
#else
    spdlog::set_level(spdlog::level::info);
#endif
    return;
  }
  std::string value(rawValue);
  std::free(rawValue);
  std::string text;
  for (char ch : value)
    text.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
  if (text == "trace")
    spdlog::set_level(spdlog::level::trace);
  else if (text == "debug")
    spdlog::set_level(spdlog::level::debug);
  else if (text == "warn" || text == "warning")
    spdlog::set_level(spdlog::level::warn);
  else if (text == "error" || text == "err")
    spdlog::set_level(spdlog::level::err);
  else if (text == "off")
    spdlog::set_level(spdlog::level::off);
  else
    spdlog::set_level(spdlog::level::info);
}

static uint16_t u16(const std::vector<uint8_t> &b, size_t o) {
  return uint16_t(b.at(o) | (b.at(o + 1) << 8));
}
static uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
  return uint32_t(b.at(o) | (b.at(o + 1) << 8) | (b.at(o + 2) << 16) |
                  (b.at(o + 3) << 24));
}
static void put16le(std::vector<uint8_t> &b, size_t o, uint16_t v) {
  b.at(o) = uint8_t(v);
  b.at(o + 1) = uint8_t(v >> 8);
}
static void put32le(std::vector<uint8_t> &b, size_t o, uint32_t v) {
  b.at(o) = uint8_t(v);
  b.at(o + 1) = uint8_t(v >> 8);
  b.at(o + 2) = uint8_t(v >> 16);
  b.at(o + 3) = uint8_t(v >> 24);
}
static uint32_t alignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }
static uint32_t alignUp(uint32_t v, uint32_t a) {
  return (v + a - 1) & ~(a - 1);
}

struct Section {
  std::string name;
  uint32_t va{}, vsize{}, raw{}, rawSize{}, chars{};
};
struct ImportSym {
  std::string dll;
  std::string name;
  uint16_t ordinal{};
  uint32_t iatRva{};
};
struct PeImage {
  fs::path path;
  std::string moduleKey;
  std::vector<uint8_t> file;
  std::vector<uint8_t> image;
  uint32_t imageBase{}, loadBase{}, entryRva{}, sizeOfImage{}, sizeOfHeaders{};
  std::array<uint32_t, 16> dirVa{};
  std::array<uint32_t, 16> dirSize{};
  std::vector<Section> sections;
  std::vector<ImportSym> imports;
  std::map<std::string, uint32_t> exportsByName;
  std::map<uint16_t, uint32_t> exportsByOrdinal;
  std::map<uint16_t, std::string> exportNamesByOrdinal;
  std::map<uint16_t, std::string> forwardersByOrdinal;
  bool relocationsStripped{};
  bool mapped{};
  bool importsBound{};
};

static std::string lowerAscii(std::string s) {
  for (char &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string moduleKeyFromPath(const fs::path &p) {
  return lowerAscii(p.filename().string());
}

static std::vector<uint8_t> readAll(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot open " + p.string());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static std::string cstr(const std::vector<uint8_t> &b, size_t off) {
  std::string s;
  while (off < b.size() && b[off])
    s.push_back(char(b[off++]));
  return s;
}
static std::optional<uint32_t> rvaToFile(const PeImage &pe, uint32_t rva) {
  if (rva < pe.sizeOfHeaders)
    return rva;
  for (auto &s : pe.sections) {
    uint32_t span = std::max(s.vsize, s.rawSize);
    if (rva >= s.va && rva < s.va + span)
      return s.raw + (rva - s.va);
  }
  return std::nullopt;
}
static std::string importName(const ImportSym &s) {
  std::ostringstream os;
  os << s.dll << "!"
     << (s.ordinal ? ("#" + std::to_string(s.ordinal)) : s.name);
  return os.str();
}

static void parseExports(PeImage &pe) {
  if (!pe.dirVa[0])
    return;
  auto expOff = rvaToFile(pe, pe.dirVa[0]);
  if (!expOff || *expOff + 40 > pe.file.size())
    return;
  const size_t d = *expOff;
  uint32_t ordinalBase = u32(pe.file, d + 16);
  uint32_t functionCount = u32(pe.file, d + 20);
  uint32_t nameCount = u32(pe.file, d + 24);
  uint32_t functionsRva = u32(pe.file, d + 28);
  uint32_t namesRva = u32(pe.file, d + 32);
  uint32_t ordinalsRva = u32(pe.file, d + 36);

  auto functionsOff = rvaToFile(pe, functionsRva);
  if (!functionsOff)
    return;

  for (uint32_t i = 0; i < functionCount; ++i) {
    size_t off = *functionsOff + i * 4;
    if (off + 4 > pe.file.size())
      break;
    uint32_t rva = u32(pe.file, off);
    if (!rva)
      continue;
    uint16_t ordinal = uint16_t(ordinalBase + i);
    if (rva >= pe.dirVa[0] && rva < pe.dirVa[0] + pe.dirSize[0]) {
      auto fwdOff = rvaToFile(pe, rva);
      if (fwdOff)
        pe.forwardersByOrdinal[ordinal] = cstr(pe.file, *fwdOff);
    } else {
      pe.exportsByOrdinal[ordinal] = rva;
    }
  }

  auto namesOff = rvaToFile(pe, namesRva);
  auto ordinalsOff = rvaToFile(pe, ordinalsRva);
  if (!namesOff || !ordinalsOff)
    return;
  for (uint32_t i = 0; i < nameCount; ++i) {
    size_t noff = *namesOff + i * 4;
    size_t ooff = *ordinalsOff + i * 2;
    if (noff + 4 > pe.file.size() || ooff + 2 > pe.file.size())
      break;
    uint32_t nameRva = u32(pe.file, noff);
    auto nameOff = rvaToFile(pe, nameRva);
    if (!nameOff)
      continue;
    std::string name = cstr(pe.file, *nameOff);
    uint16_t ordinal = uint16_t(ordinalBase + u16(pe.file, ooff));
    pe.exportNamesByOrdinal[ordinal] = name;
    auto it = pe.exportsByOrdinal.find(ordinal);
    if (it != pe.exportsByOrdinal.end())
      pe.exportsByName[lowerAscii(name)] = it->second;
  }
}

static PeImage parsePe(const fs::path &path) {
  PeImage pe;
  pe.path = path;
  pe.file = readAll(path);
  if (pe.file.size() < 0x100 || u16(pe.file, 0) != 0x5a4d)
    throw std::runtime_error("not MZ");
  uint32_t nt = u32(pe.file, 0x3c);
  if (u32(pe.file, nt) != 0x4550)
    throw std::runtime_error("not PE");
  uint16_t machine = u16(pe.file, nt + 4);
  uint16_t sections = u16(pe.file, nt + 6);
  uint16_t optSize = u16(pe.file, nt + 20);
  uint16_t characteristics = u16(pe.file, nt + 22);
  uint32_t opt = nt + 24;
  if (u16(pe.file, opt) != 0x10b)
    throw std::runtime_error("only PE32 supported");
  pe.entryRva = u32(pe.file, opt + 16);
  pe.imageBase = u32(pe.file, opt + 28);
  pe.sizeOfImage = u32(pe.file, opt + 56);
  pe.sizeOfHeaders = u32(pe.file, opt + 60);
  pe.relocationsStripped = (characteristics & 0x0001u) != 0;
  uint32_t numDirs = std::min<uint32_t>(16, u32(pe.file, opt + 92));
  for (uint32_t i = 0; i < numDirs; i++) {
    pe.dirVa[i] = u32(pe.file, opt + 96 + i * 8);
    pe.dirSize[i] = u32(pe.file, opt + 100 + i * 8);
  }
  pe.moduleKey = moduleKeyFromPath(path);
  spdlog::debug("PE {} machine=0x{:04x} imageBase=0x{:08x} entry=0x{:08x} "
                "imageSize=0x{:x}",
                pe.moduleKey, machine, pe.imageBase, pe.imageBase + pe.entryRva,
                pe.sizeOfImage);
  if (machine != 0x0166)
    spdlog::warn("expected MIPS R4000 machine 0x0166, got 0x{:04x}", machine);
  size_t sh = opt + optSize;
  for (uint16_t i = 0; i < sections; i++) {
    Section s;
    s.name = cstr(pe.file, sh + i * 40);
    s.vsize = u32(pe.file, sh + i * 40 + 8);
    s.va = u32(pe.file, sh + i * 40 + 12);
    s.rawSize = u32(pe.file, sh + i * 40 + 16);
    s.raw = u32(pe.file, sh + i * 40 + 20);
    s.chars = u32(pe.file, sh + i * 40 + 36);
    pe.sections.push_back(s);
    spdlog::info(
        "  section {:<8} va=0x{:08x} vsz=0x{:x} raw=0x{:x} rawsz=0x{:x}",
        s.name, pe.imageBase + s.va, s.vsize, s.raw, s.rawSize);
  }
  // Normal import directory.
  if (pe.dirVa[1]) {
    auto impOff = rvaToFile(pe, pe.dirVa[1]);
    if (impOff)
      for (size_t d = *impOff; d + 20 <= pe.file.size(); d += 20) {
        uint32_t oft = u32(pe.file, d), nameRva = u32(pe.file, d + 12),
                 ft = u32(pe.file, d + 16);
        if (!oft && !nameRva && !ft)
          break;
        auto no = rvaToFile(pe, nameRva);
        if (!no)
          continue;
        std::string dll = cstr(pe.file, *no);
        uint32_t thunkRva = oft ? oft : ft;
        for (uint32_t idx = 0;; idx++) {
          auto to = rvaToFile(pe, thunkRva + idx * 4);
          if (!to)
            break;
          uint32_t val = u32(pe.file, *to);
          if (!val)
            break;
          ImportSym sym;
          sym.dll = dll;
          sym.iatRva = ft + idx * 4;
          if (val & 0x80000000u)
            sym.ordinal = uint16_t(val & 0xffff);
          else {
            auto hn = rvaToFile(pe, val);
            if (hn)
              sym.name = cstr(pe.file, *hn + 2);
          }
          pe.imports.push_back(sym);
        }
      }
  }
  parseExports(pe);
  spdlog::info("imports parsed: {}", pe.imports.size());
  for (size_t i = 0; i < std::min<size_t>(pe.imports.size(), 64); ++i)
    spdlog::info("  import {} iat=0x{:08x}", importName(pe.imports[i]),
                 pe.imageBase + pe.imports[i].iatRva);
  spdlog::info("exports parsed: {} named, {} ordinal, {} forwarders",
               pe.exportsByName.size(), pe.exportsByOrdinal.size(),
               pe.forwardersByOrdinal.size());
  return pe;
}

struct Framebuffer {
  int w = 800, h = 480;
  std::vector<uint32_t> bgra =
      std::vector<uint32_t>(size_t(w * h), 0xffffffffu);
};
static void writePpm(const fs::path &p, const Framebuffer &fb, int stage) {
  auto pixels = fb.bgra;
  // Draw simple non-app status bands, explicitly diagnostic framebuffer
  // surface.
  for (int y = 0; y < fb.h; y++)
    for (int x = 0; x < fb.w; x++) {
      uint8_t shade = uint8_t(255 - std::min(200, stage * 30));
      if (y < 32)
        pixels[y * fb.w + x] = 0xff000000u | (uint32_t(30 + stage * 20) << 16) |
                               (uint32_t(30) << 8) | shade;
    }
  std::ofstream f(p, std::ios::binary);
  f << "P6\n" << fb.w << " " << fb.h << "\n255\n";
  for (uint32_t px : pixels) {
    char rgb[3] = {char((px >> 16) & 255), char((px >> 8) & 255),
                   char(px & 255)};
    f.write(rgb, 3);
  }
  spdlog::info("wrote framebuffer capture {}", p.string());
}

static void hookInvalid(uc_engine *uc, uc_mem_type type, uint64_t addr,
                        int size, int64_t value, void *) {
  uint32_t pc = 0, ra = 0, sp = 0, v0 = 0, a0 = 0, a1 = 0, s7 = 0;
  uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
  uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
  uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
  uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
  uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
  uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
  uc_reg_read(uc, UC_MIPS_REG_S7, &s7);
  spdlog::warn("unmapped memory type={} addr=0x{:08x} size={} pc=0x{:08x} "
               "ra=0x{:08x} sp=0x{:08x} v0=0x{:08x} a0=0x{:08x} a1=0x{:08x} s7=0x{:08x}",
               int(type), uint32_t(addr), size, pc, ra, sp, v0, a0, a1, s7);
  uint32_t words[8]{};
  if (a0 && uc_mem_read(uc, a0, words, sizeof(words)) == UC_ERR_OK) {
    spdlog::warn("unmapped memory a0 words: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                 words[0], words[1], words[2], words[3], words[4], words[5],
                 words[6], words[7]);
  }
  if (a1 && a1 != a0 && uc_mem_read(uc, a1, words, sizeof(words)) == UC_ERR_OK) {
    spdlog::warn("unmapped memory a1 words: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                 words[0], words[1], words[2], words[3], words[4], words[5],
                 words[6], words[7]);
  }
}

static void hookProfileSettingC3(uc_engine *uc, uint64_t address, uint32_t,
                                 void *user) {
  if (address == 0x0001d13c) {
    uint32_t ra = 0, sp = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    spdlog::info("diag profile key 0xc3 getter enter pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                 uint32_t(address), ra, sp);
    return;
  }
  if (address == 0x0001d170) {
    uint32_t value = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &value);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    spdlog::info("diag profile key 0xc3 getter return value={} (0x{:08x}) ra=0x{:08x}",
                 value, value, ra);
    return;
  }
  auto *state = static_cast<ProfileDiagState *>(user);
  if (!state) return;
  if (address == 0x0006bd18) {
    uint32_t kind = 0, pathPtr = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_A1, &kind);
    uc_reg_read(uc, UC_MIPS_REG_A2, &pathPtr);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    state->currentConfigKind = kind;
    state->currentConfigPath = readGuestWideLossy(uc, pathPtr);
    spdlog::info("diag config load enter kind={} path=\"{}\" ra=0x{:08x}",
                 int16_t(kind & 0xffffu), state->currentConfigPath, ra);
    return;
  }
  if (address == 0x0006c1a8) {
    uint32_t keyPtr = 0, dataPtr = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_A2, &keyPtr);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uint16_t key = 0;
    if (!readGuestU16(uc, keyPtr, key) || key != 0x00c3) return;
    uint16_t recordType = 0;
    uint32_t value = 0;
    readGuestU32(uc, keyPtr + 4, dataPtr);
    readGuestU16(uc, dataPtr, recordType);
    if (recordType == 3 && dataPtr) {
      readGuestU32(uc, dataPtr + 4, value);
    } else if (recordType == 4 && dataPtr) {
      readGuestU32(uc, dataPtr + 2, value);
    }
    spdlog::info("diag config insert key=0x{:04x} type={} value={} data=0x{:08x} kind={} path=\"{}\" ra=0x{:08x}",
                 key, int16_t(recordType), value, dataPtr,
                 int16_t(state->currentConfigKind & 0xffffu),
                 state->currentConfigPath, ra);
    spdlog::info("diag config insert key=0x00c3 keyobj[0..31]={} data[-16..47]={}",
                 readGuestHex(uc, keyPtr, 32),
                 dataPtr >= 16 ? readGuestHex(uc, dataPtr - 16, 64)
                               : readGuestHex(uc, dataPtr, 48));
    return;
  }
  if (address == 0x0006c084 && !state->currentConfigPath.empty()) {
    uint32_t ret = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &ret);
    spdlog::info("diag config load exit ret={} kind={} path=\"{}\"",
                 ret, int16_t(state->currentConfigKind & 0xffffu),
                 state->currentConfigPath);
    state->currentConfigPath.clear();
    state->currentConfigKind = 0;
  }
}

static void hookSerialOpenWrapper(uc_engine *uc, uint64_t address, uint32_t,
                                  void *) {
  uint32_t ra = 0, sp = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, flags = 0, extra = 0;
  uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
  uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
  uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
  uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
  uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
  uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
  if (address == 0x000e8ca8) {
    spdlog::info("diag serial port-name setter enter ra=0x{:08x} obj=0x{:08x} name=0x{:08x} \"{}\"",
                 ra, a0, a1, readGuestWideLossy(uc, a1, 64));
    return;
  }
  if (address == 0x000319b0) {
    uint32_t v0 = 0, a0 = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    spdlog::info("diag gps-port table lookup return index=0x1d value={} base=0x{:08x} slot=0x{:08x} ra=0x{:08x}",
                 int16_t(v0 & 0xffffu), a0, a0 + ((0x240u + 0x1du) * 4u), ra);
    return;
  }
  if (address != 0x0022f4f8) return;
  readGuestU32(uc, sp + 0x10, flags);
  readGuestU32(uc, sp + 0x14, extra);
  spdlog::info("diag serial open wrapper enter ra=0x{:08x} obj=0x{:08x} name=0x{:08x} \"{}\" access=0x{:08x} share=0x{:08x} flags=0x{:08x} extra=0x{:08x}",
               ra, a0, a1, readGuestWideLossy(uc, a1, 64), a2, a3, flags, extra);
}

static void hookExitCleanup(uc_engine *uc, uint64_t address, uint32_t, void *) {
  uint32_t ra = 0, sp = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, s5 = 0;
  uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
  uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
  uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
  uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
  uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
  uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
  uc_reg_read(uc, UC_MIPS_REG_S5, &s5);
  uint32_t savedRa = 0, savedS5 = 0;
  readGuestU32(uc, sp + 0x24, savedRa);
  readGuestU32(uc, sp + 0x1c, savedS5);
  if (address == 0x0048f964) {
    spdlog::warn("diag exit cleanup enter pc=0x{:08x} ra=0x{:08x} sp=0x{:08x} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x}",
                 uint32_t(address), ra, sp, a0, a1, a2, a3);
  } else if (address == 0x0048fa98) {
    spdlog::warn("diag exit cleanup epilogue pc=0x{:08x} ra=0x{:08x} sp=0x{:08x} savedRa=0x{:08x} s5=0x{:08x} savedS5=0x{:08x} stack={}",
                 uint32_t(address), ra, sp, savedRa, s5, savedS5,
                 readGuestHex(uc, sp, 0x30));
  }
}

struct ModuleLoader {
  uc_engine *uc{};
  SyntheticDllRuntime *synthetic{};
  fs::path mainExe;
  std::map<std::string, PeImage> modules;
  std::vector<fs::path> searchDirs;
  uint32_t nextDllBase = 0x50000000;

  ModuleLoader(uc_engine *uc_, SyntheticDllRuntime *synthetic_,
               fs::path mainExe_, const std::vector<fs::path> &dllSearchDirs)
      : uc(uc_), synthetic(synthetic_), mainExe(std::move(mainExe_)) {
    addSearchDir(mainExe.parent_path());
    for (const auto &dir : dllSearchDirs)
      addSearchDir(dir);
  }

  void addSearchDir(const fs::path &dir) {
    if (dir.empty())
      return;
    for (const auto &existing : searchDirs) {
      if (lowerAscii(existing.string()) == lowerAscii(dir.string()))
        return;
    }
    searchDirs.push_back(dir);
  }

  std::optional<fs::path> findModuleFile(const std::string &dllName,
                                         const fs::path &importerDir) {
    std::vector<fs::path> dirs;
    if (!importerDir.empty())
      dirs.push_back(importerDir);
    dirs.insert(dirs.end(), searchDirs.begin(), searchDirs.end());
    const std::string want = lowerAscii(fs::path(dllName).filename().string());
    for (const auto &dir : dirs) {
      std::error_code ec;
      if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        continue;
      fs::path direct = dir / dllName;
      if (fs::exists(direct, ec))
        return direct;
      for (const auto &ent : fs::directory_iterator(
               dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec)
          break;
        if (!ent.is_regular_file(ec))
          continue;
        if (lowerAscii(ent.path().filename().string()) == want)
          return ent.path();
      }
    }
    return std::nullopt;
  }

  void buildImageBytes(PeImage &pe) {
    pe.image.assign(pe.sizeOfImage, 0);
    size_t headerBytes =
        std::min<size_t>({pe.sizeOfHeaders, pe.file.size(), pe.image.size()});
    std::copy(pe.file.begin(), pe.file.begin() + headerBytes, pe.image.begin());
    for (const auto &s : pe.sections) {
      if (!s.raw || !s.rawSize || s.raw >= pe.file.size() ||
          s.va >= pe.image.size())
        continue;
      size_t n = std::min<size_t>(
          {s.rawSize, pe.file.size() - s.raw, pe.image.size() - s.va});
      std::copy(pe.file.begin() + s.raw, pe.file.begin() + s.raw + n,
                pe.image.begin() + s.va);
    }
  }

  bool tryMap(uint32_t imageBase, uint32_t imageSize, uint32_t &mappedBase,
              uint32_t &mappedSize) {
    mappedBase = alignDown(imageBase, 0x1000);
    mappedSize = alignUp((imageBase - mappedBase) + imageSize, 0x1000);
    return uc_mem_map(uc, mappedBase, mappedSize, UC_PROT_ALL) == UC_ERR_OK;
  }

  void mapImage(PeImage &pe, bool mainModule) {
    if (pe.mapped)
      return;
    buildImageBytes(pe);
    uint32_t mapBase = 0, mapSize = 0;
    uint32_t desired = pe.imageBase ? pe.imageBase : nextDllBase;
    if (!tryMap(desired, pe.sizeOfImage, mapBase, mapSize)) {
      if (mainModule)
        throw std::runtime_error("cannot map main image at preferred base");
      for (;;) {
        desired = alignUp(nextDllBase, 0x10000);
        nextDllBase = desired + alignUp(pe.sizeOfImage + 0x10000, 0x10000);
        if (tryMap(desired, pe.sizeOfImage, mapBase, mapSize))
          break;
      }
    }
    pe.loadBase = desired;
    applyRelocations(pe);
    uc_mem_write(uc, pe.loadBase, pe.image.data(), pe.image.size());
    pe.mapped = true;
    spdlog::info("mapped {:<20} preferred=0x{:08x} loaded=0x{:08x} size=0x{:x}",
                 pe.moduleKey, pe.imageBase, pe.loadBase, pe.sizeOfImage);
  }

  bool preferredRangeIsFree(const PeImage &pe) const {
    if (!pe.imageBase || !pe.sizeOfImage)
      return true;
    const uint32_t wantStart = alignDown(pe.imageBase, 0x1000);
    const uint32_t wantEnd = alignUp(pe.imageBase + pe.sizeOfImage, 0x1000);
    for (const auto &[key, module] : modules) {
      (void)key;
      if (!module.mapped || !module.loadBase || !module.sizeOfImage)
        continue;
      const uint32_t haveStart = alignDown(module.loadBase, 0x1000);
      const uint32_t haveEnd =
          alignUp(module.loadBase + module.sizeOfImage, 0x1000);
      if (wantStart < haveEnd && haveStart < wantEnd)
        return false;
    }
    return true;
  }

  void applyRelocations(PeImage &pe) {
    int64_t delta64 = int64_t(pe.loadBase) - int64_t(pe.imageBase);
    if (!delta64 || !pe.dirVa[5] || !pe.dirSize[5])
      return;
    int32_t delta = int32_t(delta64);
    uint32_t rva = pe.dirVa[5];
    uint32_t end = pe.dirVa[5] + pe.dirSize[5];
    std::set<uint16_t> warnedTypes;
    size_t applied = 0;
    while (rva + 8 <= end && rva + 8 <= pe.image.size()) {
      uint32_t pageRva = u32(pe.image, rva);
      uint32_t blockSize = u32(pe.image, rva + 4);
      if (blockSize < 8)
        break;
      uint32_t count = (blockSize - 8) / 2;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t entryOff = rva + 8 + i * 2;
        if (entryOff + 2 > pe.image.size())
          break;
        uint16_t entry = u16(pe.image, entryOff);
        uint16_t type = entry >> 12;
        uint32_t off = pageRva + (entry & 0x0fff);
        if (type == 0)
          continue;
        if (off + 4 > pe.image.size())
          continue;
        if (type == 1) {
          uint16_t value = u16(pe.image, off);
          put16le(pe.image, off,
                  uint16_t(value + uint16_t(uint32_t(delta) >> 16)));
          applied++;
        } else if (type == 2) {
          uint16_t value = u16(pe.image, off);
          put16le(pe.image, off, uint16_t(value + uint16_t(delta)));
          applied++;
        } else if (type == 3) {
          put32le(pe.image, off, u32(pe.image, off) + uint32_t(delta));
          applied++;
        } else if (type == 4) {
          if (i + 1 >= count)
            break;
          uint32_t nextEntryOff = rva + 8 + (i + 1) * 2;
          int16_t lowAdjust = int16_t(u16(pe.image, nextEntryOff));
          int32_t high = int16_t(u16(pe.image, off));
          int32_t value = (high << 16) + lowAdjust + delta;
          put16le(pe.image, off, uint16_t((value + 0x8000) >> 16));
          i++;
          applied++;
        } else if (type == 5) {
          uint32_t instr = u32(pe.image, off);
          uint32_t target = (instr & 0x03ffffffu) << 2;
          target += uint32_t(delta);
          put32le(pe.image, off,
                  (instr & 0xfc000000u) | ((target >> 2) & 0x03ffffffu));
          applied++;
        } else if (warnedTypes.insert(type).second) {
          spdlog::warn("{} has unsupported relocation type {}", pe.moduleKey,
                       type);
        }
      }
      rva += blockSize;
    }
    spdlog::info("applied {} relocations for {}", applied, pe.moduleKey);
  }

  uint32_t resolveExport(const PeImage &module, const ImportSym &sym) const {
    if (sym.ordinal) {
      auto fwd = module.forwardersByOrdinal.find(sym.ordinal);
      if (fwd != module.forwardersByOrdinal.end()) {
        spdlog::warn("forwarded export not yet bound: {}!#{} -> {}",
                     module.moduleKey, sym.ordinal, fwd->second);
        return 0;
      }
      auto it = module.exportsByOrdinal.find(sym.ordinal);
      return it == module.exportsByOrdinal.end() ? 0
                                                 : module.loadBase + it->second;
    }
    auto it = module.exportsByName.find(lowerAscii(sym.name));
    return it == module.exportsByName.end() ? 0 : module.loadBase + it->second;
  }

  PeImage *loadModuleByPath(const fs::path &path, bool mainModule = false) {
    std::string key = moduleKeyFromPath(path);
    auto existing = modules.find(key);
    if (existing != modules.end())
      return &existing->second;
    PeImage parsed = parsePe(path);
    auto [it, inserted] = modules.emplace(key, std::move(parsed));
    PeImage &pe = it->second;
    mapImage(pe, mainModule);
    bindImports(pe);
    if (synthetic) {
      synthetic->registerLoadedModule(pe.moduleKey, pe.path, pe.loadBase,
                                      pe.sizeOfImage,
                                      pe.exportsByName, pe.exportsByOrdinal);
    }
    return &pe;
  }

  void preloadDirectoryDlls(const fs::path &dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
      return;
    for (const auto &ent : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
      if (ec)
        break;
      if (!ent.is_regular_file(ec))
        continue;
      if (lowerAscii(ent.path().extension().string()) != ".dll")
        continue;
      loadModuleByPath(ent.path(), false);
    }
  }

  void preloadAvailableDlls() {
    std::set<std::string> seenDirs;
    for (const auto &dir : searchDirs) {
      if (!seenDirs.insert(lowerAscii(dir.string())).second)
        continue;
      preloadDirectoryDlls(dir);
    }
    spdlog::info("loaded PE modules: {}", modules.size());
  }

  PeImage *loadModuleByName(const std::string &dllName,
                            const fs::path &importerDir) {
    std::string key = lowerAscii(fs::path(dllName).filename().string());
    auto existing = modules.find(key);
    if (existing != modules.end())
      return &existing->second;
    auto found = findModuleFile(dllName, importerDir);
    if (!found) {
      if (synthetic) {
        if (auto syntheticModule = synthetic->createModule(key)) {
          PeImage pe;
          pe.path = fs::path("[synthetic]") / syntheticModule->moduleName;
          pe.moduleKey = syntheticModule->moduleName;
          pe.imageBase = syntheticModule->imageBase;
          pe.loadBase = syntheticModule->imageBase;
          pe.sizeOfImage = syntheticModule->imageSize;
          pe.exportsByName = syntheticModule->exportsByName;
          pe.exportsByOrdinal = syntheticModule->exportsByOrdinal;
          pe.exportNamesByOrdinal = syntheticModule->exportNamesByOrdinal;
          pe.mapped = true;
          pe.importsBound = true;
          auto [it, inserted] = modules.emplace(key, std::move(pe));
          synthetic->registerLoadedModule(
              it->second.moduleKey, it->second.path, it->second.loadBase,
              it->second.sizeOfImage,
              it->second.exportsByName, it->second.exportsByOrdinal);
          spdlog::warn("using synthetic {} because no real DLL was found in "
                       "search paths",
                       key);
          return &it->second;
        }
      }
      std::ostringstream os;
      os << "required DLL not found: " << dllName << " (searched:";
      for (const auto &dir : searchDirs)
        os << " " << dir.string();
      os << ")";
      throw std::runtime_error(os.str());
    }
    return loadModuleByPath(*found, false);
  }

  void bindImports(PeImage &pe) {
    if (pe.importsBound)
      return;
    pe.importsBound = true;
    size_t realBindings = 0;
    for (const auto &sym : pe.imports) {
      PeImage *dep = loadModuleByName(sym.dll, pe.path.parent_path());
      uint32_t target = resolveExport(*dep, sym);
      if (!target) {
        throw std::runtime_error("required export not found: " +
                                 importName(sym));
      }
      uint32_t iat = pe.loadBase + sym.iatRva;
      uc_mem_write(uc, iat, &target, 4);
      realBindings++;
    }
    if (!pe.imports.empty()) {
      spdlog::info("bound imports for {:<20} real={}", pe.moduleKey,
                   realBindings);
    }
  }

  bool launchGuestProcessInSharedRuntime(
      SyntheticDllRuntime::GuestProcessLaunch &launch) {
    if (launch.hostApplication.empty())
      return false;

    const std::string key = moduleKeyFromPath(launch.hostApplication);
    auto existing = modules.find(key);
    if (existing == modules.end()) {
      PeImage parsed = parsePe(launch.hostApplication);
      if (parsed.relocationsStripped && !preferredRangeIsFree(parsed)) {
        spdlog::info("CreateProcessW shared guest launch skipped for {}: "
                     "relocations stripped and preferred range "
                     "0x{:08x}-0x{:08x} is unavailable",
                     parsed.moduleKey, parsed.imageBase,
                     parsed.imageBase + parsed.sizeOfImage);
        return false;
      }
      auto [it, inserted] = modules.emplace(key, std::move(parsed));
      (void)inserted;
      PeImage &pe = it->second;
      mapImage(pe, false);
      bindImports(pe);
      if (synthetic) {
        synthetic->registerLoadedModule(pe.moduleKey, pe.path, pe.loadBase,
                                        pe.sizeOfImage,
                                        pe.exportsByName, pe.exportsByOrdinal);
      }
      existing = it;
    }

    PeImage &child = existing->second;
    if (child.relocationsStripped && child.loadBase != child.imageBase) {
      spdlog::info("CreateProcessW shared guest launch skipped for {}: "
                   "relocations stripped but loaded at 0x{:08x}, preferred "
                   "0x{:08x}",
                   child.moduleKey, child.loadBase, child.imageBase);
      return false;
    }
    return synthetic->startGuestProcessImage(
        launch.guestApplication, launch.hostApplication, child.loadBase,
        child.loadBase + child.entryRva, launch.commandLine,
        launch.processHandle, launch.threadHandle, launch.processId,
        launch.threadId);
  }
};

static int runImage(PeImage &pe, const std::vector<fs::path> &dllSearchDirs,
                    const std::optional<fs::path> &registryPath,
                    Framebuffer &fb,
                    const std::optional<fs::path> &serialMapPath,
                    const fs::path &sdmmcHostPath,
                    const std::wstring &guestCommandLine, bool headless,
                    uint64_t instructionLimit) {
  uc_engine *uc = nullptr;
  uc_err err = uc_open(
      UC_ARCH_MIPS,
      static_cast<uc_mode>(UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN), &uc);
  if (err)
    throw std::runtime_error(std::string("uc_open: ") + uc_strerror(err));
  auto close = [&] {
    if (uc)
      uc_close(uc);
  };
  try {
    SyntheticDllRuntime synthetic(uc);
    synthetic.setSdmmcHostPath(sdmmcHostPath);
    synthetic.setMainModulePath(pe.path.string());
    synthetic.setFramebuffer(fb.bgra.data(), fb.w, fb.h);
    if (registryPath)
      synthetic.setRegistryPath(*registryPath);
    if (serialMapPath)
      synthetic.setSerialDeviceMapPath(*serialMapPath);
    uc_hook syntheticHook{};
    uc_hook_add(uc, &syntheticHook, UC_HOOK_CODE,
                (void *)SyntheticDllRuntime::hookCode, &synthetic, 0x70000000,
                0x70ffffff);
    ModuleLoader loader(uc, &synthetic, pe.path, dllSearchDirs);
    synthetic.setGuestProcessLauncher([&](SyntheticDllRuntime::GuestProcessLaunch &launch) {
      return loader.launchGuestProcessInSharedRuntime(launch);
    });
    PeImage *main = loader.loadModuleByPath(pe.path, true);
    if (!main)
      throw std::runtime_error("failed to load main module");
    synthetic.setMainModuleBase(main->loadBase);
    ProfileDiagState profileDiag;
    uc_hook profileSettingHook{};
    uc_hook profileConfigLoadHook{};
    uc_hook profileConfigInsertHook{};
    uc_hook profileConfigExitHook{};
    uc_hook serialOpenWrapperHook{};
    uc_hook serialPortSetterHook{};
    uc_hook gpsPortLookupHook{};
    uc_hook exitCleanupHook{};
    if (lowerAscii(main->path.filename().string()) == "inavi.exe") {
      const uc_err hookErr =
          uc_hook_add(uc, &profileSettingHook, UC_HOOK_CODE,
                      (void *)hookProfileSettingC3, &profileDiag, 0x0001d13c,
                      0x0001d170);
      if (hookErr != UC_ERR_OK) {
        spdlog::warn("diag profile key 0xc3 hook failed: {} ({})",
                     int(hookErr), uc_strerror(hookErr));
      }
      uc_hook_add(uc, &profileConfigLoadHook, UC_HOOK_CODE,
                  (void *)hookProfileSettingC3, &profileDiag, 0x0006bd18,
                  0x0006bd18);
      uc_hook_add(uc, &profileConfigInsertHook, UC_HOOK_CODE,
                  (void *)hookProfileSettingC3, &profileDiag, 0x0006c1a8,
                  0x0006c1a8);
      uc_hook_add(uc, &profileConfigExitHook, UC_HOOK_CODE,
                  (void *)hookProfileSettingC3, &profileDiag, 0x0006c084,
                  0x0006c084);
      uc_hook_add(uc, &serialOpenWrapperHook, UC_HOOK_CODE,
                  (void *)hookSerialOpenWrapper, nullptr, 0x0022f4f8,
                  0x0022f4f8);
      uc_hook_add(uc, &serialPortSetterHook, UC_HOOK_CODE,
                  (void *)hookSerialOpenWrapper, nullptr, 0x000e8ca8,
                  0x000e8ca8);
      uc_hook_add(uc, &gpsPortLookupHook, UC_HOOK_CODE,
                  (void *)hookSerialOpenWrapper, nullptr, 0x000319b0,
                  0x000319b0);
      uc_hook_add(uc, &exitCleanupHook, UC_HOOK_CODE,
                  (void *)hookExitCleanup, nullptr, 0x0048f964,
                  0x0048fa9c);
    }
    loader.preloadAvailableDlls();
    constexpr uint32_t STACK_BASE = 0x0f000000, STACK_SIZE = 0x00100000;
    uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
    uint32_t sp = STACK_BASE + STACK_SIZE - 0x1000;
    uc_reg_write(uc, UC_MIPS_REG_SP, &sp);
    uint32_t gp = main->loadBase + 0x8000;
    uc_reg_write(uc, UC_MIPS_REG_GP, &gp);
    const uint32_t commandLine = STACK_BASE + 0x800;
    writeGuestCommandLine(uc, commandLine, guestCommandLine);
    const uint32_t hInstance = main->loadBase;
    const uint32_t hPrevInstance = 0;
    const uint32_t nCmdShow = 1;
    uc_reg_write(uc, UC_MIPS_REG_A0, &hInstance);
    uc_reg_write(uc, UC_MIPS_REG_A1, &hPrevInstance);
    uc_reg_write(uc, UC_MIPS_REG_A2, &commandLine);
    uc_reg_write(uc, UC_MIPS_REG_A3, &nCmdShow);
    uint32_t mainReturn = synthetic.threadExitStubAddress();
    if (mainReturn) uc_reg_write(uc, UC_MIPS_REG_RA, &mainReturn);
    uc_hook h2{};
    uc_hook_add(uc, &h2, UC_HOOK_MEM_INVALID, (void *)hookInvalid, nullptr, 1,
                0);
    uint32_t entry = main->loadBase + main->entryRva;
    spdlog::info("starting Unicorn at 0x{:08x} instructionLimit={}", entry,
                 instructionLimit);
    err = uc_emu_start(uc, entry, 0, 0, instructionLimit);
    uint32_t pc = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    spdlog::warn("emulation stopped err={} ({}) pc=0x{:08x} ra=0x{:08x}",
                 int(err), uc_strerror(err), pc, ra);
    synthetic.flushRegistry();
    synthetic.runHostMessageLoopUntilClosed(!headless);
    close();
    return err == UC_ERR_OK ? 0 : 2;
  } catch (...) {
    close();
    throw;
  }
}

int wmain(int argc, wchar_t **argv) {
  configureLogging();
  try {
    if (argc < 2) {
      spdlog::error(
          "usage: iNavi_Unicorn_Emulator.exe <primary.exe> [--registry "
          "regs.json] [--sdmmc-path host_sdmmc_dir] "
          "[--serial-map devices.json] [--guest-command-line text] [--headless] "
          "[dll_search_dir ...]");
      return 1;
    }
    fs::path exe = fs::path(argv[1]);
    std::optional<fs::path> registryPath;
    std::optional<fs::path> serialMapPath;
    std::optional<fs::path> sdmmcHostPath;
    std::wstring guestCommandLine;
    std::vector<fs::path> dllSearchDirs;
    bool headless = false;
    uint64_t instructionLimit = 50000000000ULL;
    for (int i = 2; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--registry") {
        if (i + 1 >= argc) {
          spdlog::error("--registry requires a regs.json path");
          return 1;
        }
        registryPath = fs::path(argv[++i]);
      } else if (arg == L"--serial-map") {
        if (i + 1 >= argc) {
          spdlog::error("--serial-map requires a JSON mapping path");
          return 1;
        }
        serialMapPath = fs::path(argv[++i]);
      } else if (arg == L"--sdmmc-path") {
        if (i + 1 >= argc) {
          spdlog::error("--sdmmc-path requires a host directory");
          return 1;
        }
        sdmmcHostPath = fs::path(argv[++i]);
      } else if (arg == L"--guest-command-line") {
        if (i + 1 >= argc) {
          spdlog::error("--guest-command-line requires text");
          return 1;
        }
        guestCommandLine = argv[++i];
      } else if (arg == L"--instructions") {
        if (i + 1 >= argc) {
          spdlog::error("--instructions requires a count");
          return 1;
        }
        instructionLimit = std::stoull(std::wstring(argv[++i]));
      } else if (arg == L"--headless") {
        headless = true;
      } else {
        dllSearchDirs.emplace_back(argv[i]);
      }
    }
    spdlog::info("iNavi Unicorn Emulator v2 fresh project");
    if (!sdmmcHostPath)
      sdmmcHostPath = exe.parent_path().parent_path();
    spdlog::info("target: {}", exe.string());
    if (registryPath)
      spdlog::info("registry: {}", registryPath->string());
    if (serialMapPath)
      spdlog::info("serial map: {}", serialMapPath->string());
    if (!guestCommandLine.empty())
      spdlog::info("guest command line: {}", narrowWideLossy(guestCommandLine));
    spdlog::info("sdmmc host path: {}", sdmmcHostPath->string());
    for (const auto &dir : dllSearchDirs)
      spdlog::info("dll search dir: {}", dir.string());
    auto pe = parsePe(exe);
    const bool writeFrameDumps =
#if defined(NDEBUG)
        false;
#else
        envFlagEnabled("INAVI_EMU_DUMPS");
#endif
    Framebuffer fb;
    if (writeFrameDumps)
      writePpm("frame_000_loader.ppm", fb, 0);
    int rc = runImage(pe, dllSearchDirs, registryPath, fb,
                      serialMapPath, *sdmmcHostPath, guestCommandLine, headless,
                      instructionLimit);
    if (writeFrameDumps)
      writePpm("frame_001_after_unicorn.ppm", fb, 1);
    return rc;
  } catch (const std::exception &e) {
    spdlog::error("fatal: {}", e.what());
    return 1;
  }
}
