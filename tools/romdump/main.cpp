#include "pkmn/core/compression.h"
#include "pkmn/core/extract.h"
#include "pkmn/core/narc.h"
#include "pkmn/core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pkmn;

namespace {

std::vector<u8> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: romdump <rom.nds> [--list] [--narcs] [--overlays]\n"
            "                          [--extract <dir>] [--narc-limit <n>]\n"
            "                          [--prepare-overlays <images-dir>]\n");
        return 2;
    }
    std::vector<u8> rom = read_file(argv[1]);
    if (rom.empty()) {
        std::fprintf(stderr, "error: could not read '%s'\n", argv[1]);
        return 1;
    }

    auto header = nds::parse_header(rom);
    if (!header) {
        std::fprintf(stderr, "error: file is too small to be an NDS ROM\n");
        return 1;
    }

    std::printf("title        : %s\n", header->title.c_str());
    std::printf("game code    : %s\n", header->game_code.c_str());
    std::printf("rom size     : %zu bytes\n", rom.size());
    std::printf("arm9         : off=0x%X entry=0x%X ram=0x%X size=0x%X\n", header->arm9_offset,
        header->arm9_entry, header->arm9_ram, header->arm9_size);
    std::printf("arm7         : off=0x%X entry=0x%X ram=0x%X size=0x%X\n", header->arm7_offset,
        header->arm7_entry, header->arm7_ram, header->arm7_size);
    std::printf("fnt          : off=0x%X size=0x%X\n", header->fnt_offset, header->fnt_size);
    std::printf("fat          : off=0x%X size=0x%X\n", header->fat_offset, header->fat_size);
    std::printf("arm9 overlays: off=0x%X size=0x%X (%zu entries)\n", header->ovl9_offset,
        header->ovl9_size, nds::overlay_count(rom));
    std::printf("arm7 overlays: off=0x%X size=0x%X\n", header->ovl7_offset, header->ovl7_size);

    if (header->arm9_size > 0
        && static_cast<usize>(header->arm9_offset) + header->arm9_size <= rom.size()) {
        byte_span bin = byte_span(rom).subspan(header->arm9_offset, header->arm9_size);
        try {
            auto dec = compression::blz_decompress(bin);
            std::printf("arm9 blz     : %u -> %zu\n", header->arm9_size, dec.size());
        } catch (const std::exception& e) {
            std::printf("arm9 blz     : <error: %s>\n", e.what());
        }
    }

    bool dump_overlays = false;
    bool list_files = false;
    bool list_narcs = false;
    std::string extract_dir;
    std::string prepare_overlays_dir;
    u32 narc_limit = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--overlays") {
            dump_overlays = true;
        } else if (arg == "--list") {
            list_files = true;
        } else if (arg == "--narcs") {
            list_narcs = true;
        } else if (arg == "--extract" && i + 1 < argc) {
            extract_dir = argv[++i];
        } else if (arg == "--prepare-overlays" && i + 1 < argc) {
            prepare_overlays_dir = argv[++i];
        } else if (arg == "--narc-limit" && i + 1 < argc) {
            narc_limit = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "warning: ignoring unknown argument '%s'\n", arg.c_str());
        }
    }

    if (!prepare_overlays_dir.empty()) {
        const std::filesystem::path root(prepare_overlays_dir);
        const std::filesystem::path overlay_dir = root / "overlays";
        std::error_code error;
        std::filesystem::create_directories(overlay_dir, error);
        std::ofstream manifest(root / "overlays.json", std::ios::binary | std::ios::trunc);
        manifest << "[\n";
        usize written = 0;
        usize skipped = 0;
        const usize count = nds::overlay_count(rom);
        for (usize i = 0; i < count; ++i) {
            auto ov = nds::overlay(rom, i);
            if (!ov) {
                ++skipped;
                continue;
            }
            auto data = nds::file_data(rom, ov->file_id);
            if (!data) {
                ++skipped;
                continue;
            }
            std::vector<u8> bytes;
            if (ov->compressed()) {
                try {
                    bytes = compression::blz_decompress(*data);
                } catch (const std::exception&) {
                    ++skipped;
                    continue;
                }
            } else {
                bytes.assign(data->begin(), data->end());
            }
            char name[32];
            std::snprintf(name, sizeof(name), "ov%03u.bin", ov->id);
            std::ofstream file(overlay_dir / name, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            manifest << (written ? ",\n" : "") << "  {\"file\": \"" << name
                     << "\", \"load_address\": \"0x" << std::hex << std::uppercase << ov->ram_addr
                     << std::dec << "\"}";
            ++written;
        }
        manifest << "\n]\n";
        std::printf("overlays     : %zu written, %zu skipped -> %s\n", written, skipped,
            overlay_dir.string().c_str());
        std::printf("manifest     : %s\n", (root / "overlays.json").string().c_str());
    }

    if (!extract_dir.empty()) {
        extract::Options options;
        options.narc_limit = narc_limit;
        auto result = extract::to_directory(rom, extract_dir, options);
        std::filesystem::path manifest_path
            = std::filesystem::path(extract_dir) / "manifest.sha256";
        std::ofstream manifest_file(manifest_path, std::ios::binary | std::ios::trunc);
        manifest_file << extract::manifest_text(result);
        std::printf(
            "extracted    : %zu files, %llu bytes, %d narc archives, %zu members, %zu skipped\n",
            result.files, static_cast<unsigned long long>(result.bytes), result.narc_archives,
            result.narc_members, result.skipped);
        std::printf("manifest     : %s\n", manifest_path.string().c_str());
        if (!dump_overlays && !list_files && !list_narcs) {
            return 0;
        }
    }

    if (list_narcs) {
        int archives = 0;
        int members = 0;
        for (const auto& fe : nds::list_files(rom)) {
            auto data = nds::file_data(rom, fe.file_id);
            if (!data || data->size() < 0x10) {
                continue;
            }
            byte_span d = *data;
            if (!(d[0] == 'N' && d[1] == 'A' && d[2] == 'R' && d[3] == 'C')) {
                continue;
            }
            auto arc = narc::parse(d);
            if (!arc) {
                continue;
            }
            ++archives;
            members += static_cast<int>(arc->file_count());
            std::printf("%s: %zu files, %zu named\n", fe.path.c_str(), arc->file_count(),
                arc->names.size());
            for (const auto& ne : arc->names) {
                std::printf("    %-40s id=%u\n", ne.path.c_str(), ne.file_id);
            }
        }
        std::printf("total NARC archives: %d, members: %d\n", archives, members);
        if (!dump_overlays) {
            return 0;
        }
    }

    if (list_files) {
        auto files = nds::list_files(rom);
        for (const auto& fe : files) {
            auto data = nds::file_data(rom, fe.file_id);
            std::printf("%06u %8zu %s\n", fe.file_id, data ? data->size() : 0, fe.path.c_str());
        }
        std::printf("total files: %zu\n", files.size());
        if (!dump_overlays) {
            return 0;
        }
    }

    if (!dump_overlays) {
        return 0;
    }

    usize count = nds::overlay_count(rom);
    int compressed = 0;
    for (usize i = 0; i < count; ++i) {
        auto ov = nds::overlay(rom, i);
        if (!ov) {
            continue;
        }
        std::printf("ov%03u file=%-5u addr=0x%-8X size=0x%-8X bss=0x%-6X %s", ov->id, ov->file_id,
            ov->ram_addr, ov->ram_size, ov->bss_size, ov->compressed() ? "blz" : "raw");
        auto data = nds::file_data(rom, ov->file_id);
        if (ov->compressed()) {
            ++compressed;
            if (data) {
                std::printf(" csize=0x%zX", data->size());
                try {
                    auto out = compression::blz_decompress(*data);
                    std::printf(
                        " dec=0x%zX%s", out.size(), out.size() == ov->ram_size ? "" : " MISMATCH");
                } catch (const std::exception& e) {
                    std::printf(" dec=<error: %s>", e.what());
                }
            }
        } else if (data) {
            std::printf(
                " size=0x%zX%s", data->size(), data->size() == ov->ram_size ? "" : " MISMATCH");
        }
        std::printf("\n");
    }
    std::printf("total overlays: %zu, compressed: %d\n", count, compressed);
    return 0;
}
