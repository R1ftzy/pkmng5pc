#include "pkmn/core/compression.h"
#include "pkmn/core/nds.h"
#include "pkmn/core/sha1.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pkmn;

namespace {

std::vector<u8> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool write_file(const std::filesystem::path& path, byte_span data)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(
        reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

bool write_text(const std::filesystem::path& path, const std::string& text)
{
    return write_file(path, byte_span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

u32 rd32(byte_span s, usize off)
{
    return static_cast<u32>(s[off]) | (static_cast<u32>(s[off + 1]) << 8)
        | (static_cast<u32>(s[off + 2]) << 16) | (static_cast<u32>(s[off + 3]) << 24);
}

std::string sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.push_back((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') ? l : '_');
    }
    return out;
}

// A BLZ image ends with a trailer that describes an encoded block preceded by a
// raw prefix. blz_decompress() takes that branch only when the trailing dword is
// non-zero; when it is zero it silently treats the file as "raw minus 4 bytes",
// so an uncompressed image must never be handed to it. Detect the compressed
// shape explicitly instead, and report the decoded length it implies.
//
// Layout, from the end: [.. encoded ..][hdr_len][enc_len u24 | 0x03][inc_len u32]
// Decoded length works out to (encoded-file length) + inc_len.
bool looks_blz(byte_span in, usize& decoded_size)
{
    if (in.size() < 8) {
        return false;
    }
    u32 inc_len = rd32(in, in.size() - 4);
    if (inc_len == 0) {
        return false;
    }
    u8 hdr_len = in[in.size() - 5];
    if (hdr_len < 0x08 || hdr_len > 0x0B) {
        return false;
    }
    u32 enc_len = rd32(in, in.size() - 8) & 0x00FFFFFF;
    if (enc_len < hdr_len || enc_len > in.size()) {
        return false;
    }
    decoded_size = in.size() + inc_len;
    return true;
}

struct Image {
    std::string bank;
    std::string label;
    std::string cpu;
    u32 load_address = 0;
    u32 entry_pc = 0;
    std::vector<u8> bytes;
};

// The checked-in per-title config (re/config/<title>.toml). An alias is a module
// copied at run time to a base no bank declares: runtime_start is where the copy
// lives, source_start the module's own guest address, so the same bytes are
// recompiled once and reachable at both addresses. A seed file is one CPU's
// observed Tier-3 entry points, replayed as [[entry_point]].
struct TitleAlias {
    std::string cpu;
    std::string name;
    std::string note;
    u32 runtime_start = 0;
    u32 source_start = 0;
    u32 size = 0;
};

struct TitleSeedFile {
    std::string cpu;
    std::string path;
};

// A declared replacement of one guest instruction in the emitted shard C.
// skip_call rewrites the per-instruction block of a BL/BLX to fall through to
// the next instruction; stub_return empties a function body to an immediate
// return. Addresses and semantics only: no ROM bytes.
struct TitlePatch {
    u32 address = 0;
    std::string kind;
    std::string reason;
};

struct TitleConfig {
    std::vector<TitleAlias> aliases;
    std::vector<TitleSeedFile> seed_files;
    std::vector<TitlePatch> patches;
};

struct Seed {
    u32 addr = 0;
    bool thumb = false;
};

std::string hex32(u32 value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", value);
    return buf;
}

std::string trim_copy(const std::string& value)
{
    usize begin = 0;
    usize end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string unquote(const std::string& value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

u32 parse_u32(const std::string& value)
{
    return static_cast<u32>(std::strtoul(unquote(trim_copy(value)).c_str(), nullptr, 0));
}

std::string strip_comment(const std::string& line)
{
    std::size_t hash = line.find('#');
    return trim_copy(hash == std::string::npos ? line : line.substr(0, hash));
}

std::pair<std::string, std::string> split_key_value(const std::string& text)
{
    std::size_t eq = text.find('=');
    if (eq == std::string::npos) {
        return {};
    }
    return { trim_copy(text.substr(0, eq)), text.substr(eq + 1) };
}

TitleConfig read_title_config(const std::string& path)
{
    TitleConfig config;
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "error: could not read title config '%s'\n", path.c_str());
        return config;
    }
    int table = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::string text = strip_comment(line);
        if (text.empty()) {
            continue;
        }
        if (text == "[[alias]]") {
            config.aliases.push_back({});
            table = 1;
            continue;
        }
        if (text == "[[seed_file]]") {
            config.seed_files.push_back({});
            table = 2;
            continue;
        }
        if (text == "[[patch]]") {
            config.patches.push_back({});
            table = 3;
            continue;
        }
        if (text.front() == '[') {
            table = 0;
            continue;
        }
        auto [key, value] = split_key_value(text);
        if (key.empty()) {
            continue;
        }
        if (table == 1 && !config.aliases.empty()) {
            TitleAlias& alias = config.aliases.back();
            if (key == "cpu") {
                alias.cpu = unquote(trim_copy(value));
            } else if (key == "name") {
                alias.name = unquote(trim_copy(value));
            } else if (key == "note") {
                alias.note = unquote(trim_copy(value));
            } else if (key == "runtime_start") {
                alias.runtime_start = parse_u32(value);
            } else if (key == "source_start") {
                alias.source_start = parse_u32(value);
            } else if (key == "size") {
                alias.size = parse_u32(value);
            }
        } else if (table == 2 && !config.seed_files.empty()) {
            TitleSeedFile& seed = config.seed_files.back();
            if (key == "cpu") {
                seed.cpu = unquote(trim_copy(value));
            } else if (key == "path") {
                seed.path = unquote(trim_copy(value));
            }
        } else if (table == 3 && !config.patches.empty()) {
            TitlePatch& patch = config.patches.back();
            if (key == "address") {
                patch.address = parse_u32(value);
            } else if (key == "kind") {
                patch.kind = unquote(trim_copy(value));
            } else if (key == "reason") {
                patch.reason = unquote(trim_copy(value));
            }
        }
    }
    return config;
}

std::vector<Seed> read_seeds(const std::filesystem::path& path)
{
    std::vector<Seed> seeds;
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "error: could not read seed file '%s'\n", path.string().c_str());
        return seeds;
    }
    bool inside = false;
    Seed current;
    auto flush = [&] {
        if (inside) {
            seeds.push_back(current);
        }
    };
    std::string line;
    while (std::getline(file, line)) {
        std::string text = strip_comment(line);
        if (text.empty()) {
            continue;
        }
        if (text == "[[entry_point]]") {
            flush();
            inside = true;
            current = Seed {};
            continue;
        }
        if (text.front() == '[') {
            flush();
            inside = false;
            continue;
        }
        auto [key, value] = split_key_value(text);
        if (key == "addr") {
            current.addr = parse_u32(value);
        } else if (key == "mode") {
            current.thumb = unquote(trim_copy(value)) == "thumb";
        }
    }
    flush();
    return seeds;
}

std::string hex8(u32 value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", value);
    return buf;
}

std::optional<std::string> read_text(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<std::string> split_whitespace(const std::string& text)
{
    std::vector<std::string> tokens;
    usize at = 0;
    while (at < text.size()) {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) {
            ++at;
        }
        usize begin = at;
        while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) {
            ++at;
        }
        if (at > begin) {
            tokens.push_back(text.substr(begin, at - begin));
        }
    }
    return tokens;
}

std::string join_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (usize i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

// One per-instruction block of an emitted shard: the L_<address>: label up to
// the next label, or up to the closing brace when the block ends the function.
struct InstructionBlock {
    usize start = 0;
    usize end = 0;
    bool ends_with_function = false;
};

std::optional<InstructionBlock> find_instruction_block(const std::string& text, u32 address)
{
    usize start = text.find("\nL_" + hex8(address) + ":");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    ++start;
    usize next_label = text.find("\nL_", start);
    usize function_end = text.find("\n}", start);
    if (function_end == std::string::npos) {
        function_end = text.size();
    }
    if (next_label < function_end) {
        return InstructionBlock { start, next_label, false };
    }
    return InstructionBlock { start, function_end, true };
}

enum class PatchOutcome { NotPresent, Applied, Malformed };

std::string patch_marker(const TitlePatch& patch)
{
    std::string reason = patch.reason;
    for (usize at = reason.find("*/"); at != std::string::npos; at = reason.find("*/", at)) {
        reason.replace(at, 2, "* /");
    }
    return "[patch " + patch.kind + ": " + reason + "]";
}

PatchOutcome patch_skip_call(std::string& text, const TitlePatch& patch, std::string& error)
{
    std::optional<InstructionBlock> block = find_instruction_block(text, patch.address);
    if (!block) {
        return PatchOutcome::NotPresent;
    }
    usize comment_begin = text.find("/*", block->start);
    usize comment_end
        = comment_begin == std::string::npos ? std::string::npos : text.find("*/", comment_begin);
    if (comment_end == std::string::npos || comment_end > block->end) {
        error = "instruction comment not found";
        return PatchOutcome::Malformed;
    }
    std::string comment = text.substr(comment_begin + 2, comment_end - comment_begin - 2);
    std::string mode;
    std::string mnemonic;
    std::vector<std::string> tokens = split_whitespace(comment);
    for (usize i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "A" || tokens[i] == "T") {
            mode = tokens[i];
            mnemonic = tokens[i + 1];
            break;
        }
    }
    if (mnemonic != "bl" && mnemonic != "blx") {
        error = "expected an unconditional BL/BLX, found '"
            + (mnemonic.empty() ? std::string("unknown") : mnemonic) + "'";
        return PatchOutcome::Malformed;
    }
    std::string addr_label = hex8(patch.address);
    u32 next = patch.address + 4;
    const std::string lr_marker = "g_cpu.R[14] = 0x";
    usize lr = text.find(lr_marker, block->start);
    if (lr != std::string::npos && lr + lr_marker.size() + 8 <= block->end) {
        next = static_cast<u32>(
            std::strtoul(text.substr(lr + lr_marker.size(), 8).c_str(), nullptr, 16));
    }
    std::string next_label = hex8(next);
    std::vector<std::string> lines;
    lines.push_back("L_" + addr_label + ":");
    lines.push_back("    /*" + comment + "  " + patch_marker(patch) + " */");
    lines.push_back("    g_cpu.R[15] = 0x" + addr_label + "u;");
    lines.push_back("    if (runtime_should_yield()) return;");
    lines.push_back("    ++g_insn_count[g_nds_active];");
    lines.push_back("    if (g_insn_hook_armed) runtime_insn_slow();");
    lines.push_back("    uint32_t _cyc_" + addr_label + " = 3u;");
    lines.push_back("    uint32_t _code_" + addr_label + " = nds_code_numc(0x" + addr_label
        + "u, NDS_ARM9_CODE_K(0x" + addr_label + "u, " + (mode == "T" ? "1" : "0") + "));");
    lines.push_back("    uint32_t _data_" + addr_label + " = 0u;");
    lines.push_back("    uint32_t _int_" + addr_label + " = 0u;");
    lines.push_back("    g_cpu.R[15] = 0x" + next_label + "u;");
    lines.push_back("    runtime_tick(g_nds_active == NDS_ARM9 ? arm9_cycle_combine(_code_"
        + addr_label + ", _data_" + addr_label + ", _int_" + addr_label
        + ", 0u) : arm7_cycle_combine(_cyc_" + addr_label + ", _data_" + addr_label
        + ", 0u, (_int_" + addr_label + " != 0u)));");
    lines.push_back("    if (runtime_unwinding()) return;");
    if (block->ends_with_function) {
        lines.push_back("    /* fall-through to 0x" + next_label + " */");
        lines.push_back("    g_cpu.R[15] = 0x" + next_label + "u;");
        lines.push_back("    { static NdsLinkSlot _lnk_" + next_label + "_f = {0, 0u, 0x"
            + next_label + "u, 0};");
        lines.push_back("      runtime_link_fallthrough(&_lnk_" + next_label + "_f); }");
        lines.push_back("    return;");
    }
    text.replace(block->start, block->end - block->start, join_lines(lines));
    return PatchOutcome::Applied;
}

PatchOutcome patch_stub_return(std::string& text, const TitlePatch& patch, std::string& error)
{
    std::string name = "_afunc_" + hex8(patch.address) + "(void) {";
    usize begin = text.find(name);
    if (begin == std::string::npos) {
        return PatchOutcome::NotPresent;
    }
    usize end = text.find("\n}", begin);
    if (end == std::string::npos) {
        error = "function body is not terminated";
        return PatchOutcome::Malformed;
    }
    std::string addr_label = hex8(patch.address);
    std::vector<std::string> lines;
    lines.push_back(name);
    lines.push_back("    /* " + patch_marker(patch) + " */");
    lines.push_back("    g_cpu.R[15] = 0x" + addr_label + "u;");
    lines.push_back("    if (runtime_should_yield()) return;");
    lines.push_back("    ++g_insn_count[g_nds_active];");
    lines.push_back("    if (g_insn_hook_armed) runtime_insn_slow();");
    lines.push_back("    uint32_t _bxt_" + addr_label + " = g_cpu.R[14];");
    lines.push_back("    g_cpu.R[15] = _bxt_" + addr_label + " & ((_bxt_" + addr_label
        + " & 1u) ? ~1u : ~3u);");
    lines.push_back("    if (_bxt_" + addr_label + " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;");
    lines.push_back("    runtime_tick(g_nds_active == NDS_ARM9 ? arm9_refill_cycles(_bxt_"
        + addr_label + ") : arm7_refill_cycles(_bxt_" + addr_label + "));");
    lines.push_back("    if (runtime_unwinding()) return;");
    lines.push_back("    if (runtime_call_should_return(g_cpu.R[15])) return;");
    lines.push_back("    runtime_dispatch_with_exchange(_bxt_" + addr_label + ");");
    lines.push_back("    return;");
    lines.push_back("}");
    text.replace(begin, end + 2 - begin, join_lines(lines));
    return PatchOutcome::Applied;
}

bool apply_bank_patches(const std::filesystem::path& bank_dir, const std::string& bank,
    const std::vector<TitlePatch>& patches, std::vector<bool>& applied)
{
    if (patches.empty()) {
        return true;
    }
    std::vector<std::filesystem::path> shards;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
        std::filesystem::directory_iterator(bank_dir, ec)) {
        std::string name = entry.path().filename().string();
        if (!entry.is_regular_file() || !name.starts_with(bank + "_")) {
            continue;
        }
        std::string suffix = name.substr(bank.size() + 1);
        if (suffix.size() == 4 && std::isdigit(static_cast<unsigned char>(suffix[0]))
            && std::isdigit(static_cast<unsigned char>(suffix[1])) && suffix[2] == '.'
            && suffix[3] == 'c') {
            shards.push_back(entry.path());
        }
    }
    std::sort(shards.begin(), shards.end());
    for (const std::filesystem::path& shard : shards) {
        std::optional<std::string> text = read_text(shard);
        if (!text) {
            std::fprintf(stderr, "error: could not read '%s'\n", shard.string().c_str());
            return false;
        }
        bool changed = false;
        for (usize i = 0; i < patches.size(); ++i) {
            if (applied[i]) {
                continue;
            }
            const TitlePatch& patch = patches[i];
            std::string error;
            PatchOutcome outcome = patch.kind == "stub_return"
                ? patch_stub_return(*text, patch, error)
                : patch_skip_call(*text, patch, error);
            if (outcome == PatchOutcome::Malformed) {
                std::fprintf(stderr, "error: patch %s %s in %s: %s\n", hex32(patch.address).c_str(),
                    patch.kind.c_str(), shard.filename().string().c_str(), error.c_str());
                return false;
            }
            if (outcome == PatchOutcome::Applied) {
                applied[i] = true;
                changed = true;
                std::printf("[patch] %s %s applied (shard %s)\n", hex32(patch.address).c_str(),
                    patch.kind.c_str(), shard.filename().string().c_str());
            }
        }
        if (changed && !write_text(shard, *text)) {
            std::fprintf(stderr, "error: could not write '%s'\n", shard.string().c_str());
            return false;
        }
    }
    return true;
}

std::string toml_for(const Image& image, const std::string& source_note,
    const std::vector<TitleAlias>& aliases, const std::vector<Seed>& seeds)
{
    std::string out;
    out += "# Generated by pkmn_bankgen from " + source_note + "; do not edit.\n";
    out += "# Regenerate with: pkmn_bankgen <rom.nds> <out-dir> --recompiler <nds_recompile>\n";
    out += "#                  --title-config re/config/<title>.toml\n";
    out += "\n[program]\n";
    out += "name         = \"" + image.label + "\"\n";
    out += "id           = \"" + image.bank + "\"\n";
    out += "load_address = " + hex32(image.load_address) + "\n";
    out += "size         = " + hex32(static_cast<u32>(image.bytes.size())) + "\n";
    out += "entry_pc     = " + hex32(image.entry_pc) + "\n";
    out += "\n[identity]\n";
    out += "sha1 = \"" + sha1::to_hex(sha1::compute(byte_span(image.bytes))) + "\"\n";
    for (const TitleAlias& alias : aliases) {
        out += "\n[[code_copy]]\n";
        if (!alias.note.empty()) {
            out += "# " + alias.note + "\n";
        }
        out += "runtime_start = " + hex32(alias.runtime_start) + "\n";
        out += "source_start  = " + hex32(alias.source_start) + "\n";
        out += "size          = " + hex32(alias.size) + "\n";
        out += "name          = \"" + (alias.name.empty() ? std::string("alias") : alias.name)
            + "\"\n";
    }
    if (!seeds.empty()) {
        out += "\n# " + std::to_string(seeds.size())
            + " entry point(s) observed in Tier 3; seeds only, no bytes.\n";
    }
    for (const Seed& seed : seeds) {
        out += "\n[[entry_point]]\n";
        out += "addr = " + hex32(seed.addr) + "\n";
        out += "mode = \"" + std::string(seed.thumb ? "thumb" : "arm") + "\"\n";
    }
    return out;
}

bool run(const std::string& command)
{
    std::printf("  $ %s\n", command.c_str());
#ifdef _WIN32
    // cmd.exe mis-parses a command whose first token is quoted; wrapping the
    // whole line in one more pair of quotes is the documented workaround.
    std::string line = "\"" + command + "\"";
#else
    const std::string& line = command;
#endif
    int rc = std::system(line.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "error: command failed (exit %d)\n", rc);
        return false;
    }
    return true;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string command_path(std::string path)
{
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return quote(path);
}

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: pkmn_bankgen <rom.nds> <out-dir> [--recompiler <nds_recompile>]\n"
            "                    [--title-config <re/config/<title>.toml>]\n"
            "                    [--shards <n>] [--emit-only]\n"
            "\n"
            "Writes decoded ARM9/ARM7 images and per-bank recompiler configs, then\n"
            "(unless --emit-only) runs the recompiler once per bank. The generated\n"
            "bank directory is the one to pass as NDS_TITLE_BANK_DIR. --title-config\n"
            "adds the checked-in per-title aliases ([[code_copy]]), observed Tier-3\n"
            "entry points ([[entry_point]]), and instruction patches ([[patch]]).\n");
        return 2;
    }

    std::string rom_path = argv[1];
    std::filesystem::path out_dir = argv[2];
    std::string recompiler;
    std::string title_config_path;
    int shards = 8;
    bool emit_only = false;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--recompiler" && i + 1 < argc) {
            recompiler = argv[++i];
        } else if (arg == "--shards" && i + 1 < argc) {
            shards = std::atoi(argv[++i]);
        } else if (arg == "--title-config" && i + 1 < argc) {
            title_config_path = argv[++i];
        } else if (arg == "--emit-only") {
            emit_only = true;
        } else {
            std::fprintf(stderr, "warning: ignoring unknown argument '%s'\n", arg.c_str());
        }
    }

    if (shards < 2) {
        // The runner globs bank bodies as *_[0-9][0-9].c; --shards 1 emits
        // <bank>.c and would be silently ignored.
        std::fprintf(stderr, "error: --shards must be >= 2 (the runner globs *_NN.c)\n");
        return 2;
    }

    std::vector<u8> rom = read_file(rom_path);
    if (rom.empty()) {
        std::fprintf(stderr, "error: could not read '%s'\n", rom_path.c_str());
        return 1;
    }

    auto header = nds::parse_header(rom);
    if (!header) {
        std::fprintf(stderr, "error: file is too small to be an NDS ROM\n");
        return 1;
    }

    std::string code = sanitize(header->game_code.empty() ? "title" : header->game_code);
    std::printf("rom          : %s\n", rom_path.c_str());
    std::printf("title        : %s (%s)\n", header->title.c_str(), header->game_code.c_str());
    std::printf("rom sha1     : %s\n", sha1::to_hex(sha1::compute(rom)).c_str());

    auto load_image = [&](const char* kind, u32 offset, u32 size, u32 load_address, u32 entry_pc,
                          const std::string& label) -> std::optional<Image> {
        if (size == 0 || static_cast<usize>(offset) + size > rom.size()) {
            std::fprintf(stderr, "error: %s range [0x%X,0x%X) is outside the ROM\n", kind, offset,
                offset + size);
            return std::nullopt;
        }
        byte_span raw = byte_span(rom).subspan(offset, size);
        usize decoded_size = 0;
        bool compressed = looks_blz(raw, decoded_size);
        Image image;
        image.bank = code + "_" + kind + "_main";
        image.label = header->title + " " + label;
        image.cpu = kind;
        image.load_address = load_address;
        image.entry_pc = entry_pc;
        if (compressed) {
            image.bytes = compression::blz_decompress(raw);
            std::printf("%-12s : blz 0x%X -> 0x%zX (predicted 0x%zX)\n", kind, size,
                image.bytes.size(), decoded_size);
            if (image.bytes.size() != decoded_size) {
                std::fprintf(stderr, "error: %s BLZ decoded to an unexpected length\n", kind);
                return std::nullopt;
            }
        } else {
            image.bytes.assign(raw.begin(), raw.end());
            std::printf("%-12s : raw 0x%zX bytes\n", kind, image.bytes.size());
        }
        std::printf("%-12s : bank=%s load=0x%X entry=0x%X sha1=%s\n", kind, image.bank.c_str(),
            load_address, entry_pc, sha1::to_hex(sha1::compute(byte_span(image.bytes))).c_str());
        return image;
    };

    std::vector<Image> images;
    auto arm9 = load_image("arm9", header->arm9_offset, header->arm9_size, header->arm9_ram,
        header->arm9_entry, "ARM9");
    if (!arm9) {
        return 1;
    }
    images.push_back(std::move(*arm9));
    auto arm7 = load_image("arm7", header->arm7_offset, header->arm7_size, header->arm7_ram,
        header->arm7_entry, "ARM7");
    if (!arm7) {
        return 1;
    }
    images.push_back(std::move(*arm7));

    std::filesystem::path bin_dir = out_dir / "bin";
    std::filesystem::path config_dir = out_dir / "config";
    // out_dir doubles as the bank group directory the runner globs; the decoded
    // images and configs live in subdirectories that glob ignores.
    std::filesystem::path bank_dir = out_dir;

    std::string source_note = std::filesystem::path(rom_path).filename().string();
    TitleConfig title;
    if (!title_config_path.empty()) {
        title = read_title_config(title_config_path);
        for (const TitlePatch& patch : title.patches) {
            if (patch.kind != "skip_call" && patch.kind != "stub_return") {
                std::fprintf(stderr, "error: patch %s has unknown kind '%s'\n",
                    hex32(patch.address).c_str(), patch.kind.c_str());
                return 2;
            }
        }
        std::printf("title config : %s (%zu alias(es), %zu seed file(s), %zu patch(es))\n",
            title_config_path.c_str(), title.aliases.size(), title.seed_files.size(),
            title.patches.size());
    }
    std::filesystem::path title_dir = std::filesystem::path(title_config_path).parent_path();

    for (const Image& image : images) {
        std::vector<TitleAlias> aliases;
        for (const TitleAlias& alias : title.aliases) {
            if (alias.cpu == image.cpu) {
                aliases.push_back(alias);
            }
        }
        std::vector<Seed> seeds;
        usize dropped = 0;
        for (const TitleSeedFile& seed_file : title.seed_files) {
            if (seed_file.cpu != image.cpu) {
                continue;
            }
            for (const Seed& seed : read_seeds(title_dir / seed_file.path)) {
                const bool in_image = seed.addr >= image.load_address
                    && (seed.addr - image.load_address) < image.bytes.size();
                bool in_copy = false;
                for (const TitleAlias& alias : aliases) {
                    if (seed.addr >= alias.runtime_start
                        && (seed.addr - alias.runtime_start) < alias.size) {
                        in_copy = true;
                    }
                }
                // A seed outside every declared range would be accepted but never
                // decoded; the recompiler only rejects one that carries a size.
                if (in_image || in_copy) {
                    seeds.push_back(seed);
                } else {
                    ++dropped;
                }
            }
        }
        std::printf("%-4s         : %zu code_copy, %zu seed(s), %zu dropped outside image\n",
            image.cpu.c_str(), aliases.size(), seeds.size(), dropped);
        std::filesystem::path bin_path = bin_dir / (image.bank + ".bin");
        std::filesystem::path config_path = config_dir / (image.bank + ".toml");
        if (!write_file(bin_path, image.bytes)) {
            std::fprintf(stderr, "error: could not write '%s'\n", bin_path.string().c_str());
            return 1;
        }
        if (!write_text(config_path, toml_for(image, source_note, aliases, seeds))) {
            std::fprintf(stderr, "error: could not write '%s'\n", config_path.string().c_str());
            return 1;
        }
        std::printf(
            "wrote        : %s + %s\n", bin_path.string().c_str(), config_path.string().c_str());
    }

    std::error_code ec;
    std::filesystem::create_directories(bank_dir, ec);
    std::printf("bank dir     : %s (pass as NDS_TITLE_BANK_DIR)\n",
        std::filesystem::absolute(bank_dir).string().c_str());

    if (emit_only || recompiler.empty()) {
        if (!emit_only) {
            std::printf("\nno --recompiler given; emitting only. Run:\n");
            for (const Image& image : images) {
                std::printf("  nds_recompile --config %s --bin %s --out %s --bank %s --shards %d\n",
                    (config_dir / (image.bank + ".toml")).string().c_str(),
                    (bin_dir / (image.bank + ".bin")).string().c_str(), bank_dir.string().c_str(),
                    image.bank.c_str(), shards);
            }
        }
        return 0;
    }

    std::vector<bool> patches_applied(title.patches.size(), false);
    for (const Image& image : images) {
        std::printf("recompile    : %s\n", image.bank.c_str());
        std::string command = command_path(recompiler);
        command += " --config " + command_path((config_dir / (image.bank + ".toml")).string());
        command += " --bin " + command_path((bin_dir / (image.bank + ".bin")).string());
        command += " --out " + command_path(bank_dir.string());
        command += " --bank " + image.bank;
        char shard_arg[32];
        std::snprintf(shard_arg, sizeof(shard_arg), " --shards %d", shards);
        command += shard_arg;
        if (!run(command)) {
            return 1;
        }
        if (!apply_bank_patches(bank_dir, image.bank, title.patches, patches_applied)) {
            return 1;
        }
    }

    usize applied_count = 0;
    for (usize i = 0; i < title.patches.size(); ++i) {
        if (patches_applied[i]) {
            ++applied_count;
            continue;
        }
        std::fprintf(stderr, "error: patch %s %s was not found in any emitted shard\n",
            hex32(title.patches[i].address).c_str(), title.patches[i].kind.c_str());
    }
    if (applied_count != title.patches.size()) {
        return 1;
    }

    std::printf("\ndone: %zu bank(s), %zu patch(es) applied in %s\n", images.size(), applied_count,
        std::filesystem::absolute(bank_dir).string().c_str());
    return 0;
}
