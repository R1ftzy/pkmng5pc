// Generates the Vulkan GLSL variants of the vendored melonDS compute
// rasterizer. The melonDS shader source is one C++ header of GLSL snippets;
// re-typing it here would drift immediately. This tool includes that header,
// assembles the same per-variant source the OpenGL compute renderer builds,
// and applies the small, mechanical set of changes Vulkan needs:
//
//   * explicit descriptor sets for every buffer, texture and storage image;
//   * the setup-index texture buffer becomes a uvec4 storage buffer;
//   * Rasterise's loose uniforms become a push-constant block;
//   * FinalPass writes the attribute plane the native sink reads back.
//
// Blueprint/build only. The generated .comp files live in the build tree.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "GPU3D_Compute_shaders.h"

namespace {

struct ShaderSpec {
    std::string name;
    const std::string* source = nullptr;
    std::vector<std::string> defines;
};

const std::map<std::string, const std::string*>& shader_sources()
{
    namespace S = melonDS::ComputeRendererShaders;
    static const std::map<std::string, const std::string*> sources = {
        { "InterpSpans", &S::InterpSpans },
        { "BinCombined", &S::BinCombined },
        { "DepthBlend", &S::DepthBlend },
        { "Rasterise", &S::Rasterise },
        { "ClearCoarseBinMask", &S::ClearCoarseBinMask },
        { "ClearIndirectWorkCount", &S::ClearIndirectWorkCount },
        { "CalcOffsets", &S::CalcOffsets },
        { "SortWork", &S::SortWork },
        { "FinalPass", &S::FinalPass },
    };
    return sources;
}

std::string trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find(separator, start);
        if (at == std::string::npos) {
            parts.push_back(trim(text.substr(start)));
            return parts;
        }
        parts.push_back(trim(text.substr(start, at - start)));
        start = at + 1;
    }
}

bool parse_spec_file(const char* path, std::vector<ShaderSpec>& out)
{
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "[ds-shader-gen] cannot open %s\n", path);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::vector<std::string> fields = split(line, '|');
        if (fields.size() != 3) {
            std::fprintf(stderr, "[ds-shader-gen] malformed spec line: %s\n", line.c_str());
            return false;
        }
        const auto source = shader_sources().find(fields[1]);
        if (source == shader_sources().end()) {
            std::fprintf(stderr, "[ds-shader-gen] unknown source symbol: %s\n", fields[1].c_str());
            return false;
        }
        ShaderSpec spec;
        spec.name = fields[0];
        spec.source = source->second;
        for (const std::string& define : split(fields[2], ',')) {
            if (!define.empty()) {
                spec.defines.push_back(define);
            }
        }
        out.push_back(std::move(spec));
    }
    if (out.empty()) {
        std::fprintf(stderr, "[ds-shader-gen] no shaders listed in %s\n", path);
        return false;
    }
    return true;
}

bool replace_all(std::string& text, const std::string& from, const std::string& to)
{
    bool changed = false;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

bool replace_once_required(
    std::string& text, const std::string& from, const std::string& to, const char* name)
{
    const std::size_t pos = text.find(from);
    if (pos == std::string::npos) {
        std::fprintf(stderr, "[ds-shader-gen] %s: missing pattern: %s\n", name, from.c_str());
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

const char* kAntiAliasingOriginal = R"(#ifdef AntiAliasing
    // resolve anti-aliasing
    if ((attr.x & 0x3U) != 0)
    {
        uint coverage = (attr.x >> 8) & 0x1FU;

        if (coverage != 0)
        {
            uint topRB = color.x & 0x3F003FU;
            uint topG = color.x & 0x003F00U;
            uint topA = bitfieldExtract(color.x, 24, 5);

            uint botRB = color.y & 0x3F003FU;
            uint botG = color.y & 0x003F00U;
            uint botA = bitfieldExtract(color.y, 24, 5);

            coverage++;

            if (botA > 0)
            {
                topRB = ((topRB * coverage) + (botRB * (32-coverage))) >> 5;
                topG = ((topG * coverage) + (botG * (32-coverage))) >> 5;

                topRB &= 0x3F003FU;
                topG &= 0x003F00U;
            }

            topA = ((topA * coverage) + (botA * (32-coverage))) >> 5;

            color.x = topRB | topG | (topA << 24);
        }
        else
        {
            color.x = color.y;
        }
    }
#endif
)";

const char* kAntiAliasingFixed = R"(#ifdef AntiAliasing
    // resolve anti-aliasing
    if ((attr.x & 0xFU) != 0)
    {
        uint coverage = (attr.x >> 8) & 0x1FU;
        if ((attr.x & 0x3U) == 0U && coverage == 0U)
            coverage = 0x1FU;

        if (coverage != 0x1FU)
        {
            if (coverage != 0)
            {
                uint topRB = color.x & 0x3F003FU;
                uint topG = color.x & 0x003F00U;
                uint topA = bitfieldExtract(color.x, 24, 5);

                uint botRB = color.y & 0x3F003FU;
                uint botG = color.y & 0x003F00U;
                uint botA = bitfieldExtract(color.y, 24, 5);

                coverage++;

                if (botA > 0)
                {
                    topRB = ((topRB * coverage) + (botRB * (32-coverage))) >> 5;
                    topG = ((topG * coverage) + (botG * (32-coverage))) >> 5;

                    topRB &= 0x3F003FU;
                    topG &= 0x003F00U;
                }

                topA = ((topA * coverage) + (botA * (32-coverage))) >> 5;

                color.x = topRB | topG | (topA << 24);
            }
            else
            {
                color.x = color.y;
            }
        }
    }
#endif
)";

bool transform_common(std::string& text, const char* name)
{
    bool ok = true;
    ok &= replace_once_required(text, "layout (std140, binding = 0) uniform MetaUniform",
        "layout (std140, set = 0, binding = 0) uniform MetaUniform", name);
    replace_all(text, "layout (std430, binding = 0) readonly buffer PolygonBuffer",
        "layout (std430, set = 0, binding = 1) readonly buffer PolygonBuffer");
    replace_all(text, "layout (std430, binding = 6) buffer BinResultBuffer",
        "layout (std430, set = 0, binding = 5) buffer BinResultBuffer");
    replace_all(text, "layout (std430, binding = 7) buffer WorkDescBuffer",
        "layout (std430, set = 0, binding = 6) buffer WorkDescBuffer");
    return ok;
}

bool transform_interp(std::string& text, const char* name)
{
    bool ok = true;
    ok &= replace_once_required(text,
        "layout (binding = 0, rgba16ui) uniform readonly uimageBuffer SetupIndices;",
        "layout (set = 0, binding = 10, std430) readonly buffer SetupIndexBuffer\n"
        "{\n"
        "    uvec4 SetupIndices[];\n"
        "};",
        name);
    ok &= replace_once_required(text,
        "uvec4 setup = imageLoad(SetupIndices, int(gl_GlobalInvocationID.x));",
        "uvec4 setup = SetupIndices[int(gl_GlobalInvocationID.x)];", name);
    ok &= replace_once_required(text,
        "        EdgeParams_YMajor(false, dxr, spanL, edgeLenL, xspan.EdgeCovL);\n"
        "        EdgeParams_YMajor(true, dxl, spanR, edgeLenR, xspan.EdgeCovR);\n",
        "        if (spanL.Increment > 0x40000)\n"
        "            EdgeParams_XMajor(true, dxr, spanL, edgeLenL, xspan.EdgeCovL);\n"
        "        else\n"
        "            EdgeParams_YMajor(true, dxr, spanL, edgeLenL, xspan.EdgeCovL);\n"
        "        if (spanR.Increment > 0x40000)\n"
        "            EdgeParams_XMajor(false, dxl, spanR, edgeLenR, xspan.EdgeCovR);\n"
        "        else\n"
        "            EdgeParams_YMajor(false, dxl, spanR, edgeLenR, xspan.EdgeCovR);\n"
        "        edgeLenL = 1;\n"
        "        edgeLenR = 1;\n",
        name);
    return ok;
}

bool transform_rasterise(std::string& text, const char* name)
{
    bool ok = true;
    ok &= replace_once_required(text,
        "layout (binding = 0) uniform usampler2DArray CurrentTexture;",
        "layout (set = 1, binding = 0) uniform usampler2DArray CurrentTexture;", name);
    ok &= replace_once_required(text,
        "layout (location = 0) uniform uint CurVariant;\n"
        "layout (location = 1) uniform vec2 InvTextureSize;",
        "__DS_PUSH_CONSTANT_DECLARATION__", name);
    replace_all(text, "CurVariant", "pc.CurVariant");
    replace_all(text, "InvTextureSize", "pc.InvTextureSize");
    ok &= replace_once_required(text, "__DS_PUSH_CONSTANT_DECLARATION__",
        "layout (push_constant) uniform PushConstants\n"
        "{\n"
        "    uint CurVariant;\n"
        "    vec2 InvTextureSize;\n"
        "} pc;",
        name);
    return ok;
}

bool transform_final(std::string& text, const char* name)
{
    bool ok = true;
    ok &= replace_once_required(text,
        "layout (binding = 0, rgba8) writeonly uniform image2D FinalFB; \n"
        "layout (binding = 1, rgba8ui) writeonly uniform uimage2D LowResFB; ",
        "layout (set = 2, binding = 0, rgba8) writeonly uniform image2D FinalFB;\n"
        "layout (set = 2, binding = 1, rgba8ui) writeonly uniform uimage2D LowResFB;\n"
        "layout (set = 2, binding = 2, r32ui) writeonly uniform uimage2D AttrFB;",
        name);
    ok &= replace_once_required(text, kAntiAliasingOriginal, kAntiAliasingFixed, name);
    ok &= replace_once_required(text,
        "        imageStore(LowResFB, lowresCoordinate, color8);\n"
        "    }\n"
        "}",
        "        imageStore(LowResFB, lowresCoordinate, color8);\n"
        "    }\n"
        "    uint outAttr = attr.x;\n"
        "#ifdef AntiAliasing\n"
        "    if ((outAttr & 0xFU) != 0U && (outAttr & 0x3U) == 0U)\n"
        "        outAttr |= 0x1F00U;\n"
        "#else\n"
        "    outAttr &= ~0x1F00U;\n"
        "#endif\n"
        "    imageStore(AttrFB, ivec2(gl_GlobalInvocationID.xy), uvec4(outAttr));\n"
        "}",
        name);
    return ok;
}

bool build_source(const ShaderSpec& spec, std::string& out)
{
    out = "#version 450\n";
    for (const std::string& define : spec.defines) {
        out += "#define ";
        out += define;
        out += "\n";
    }
    out += "#define ScreenWidth 256\n";
    out += "#define ScreenHeight 192\n";
    out += "#define RenderScale 1\n";
    out += "#define MaxWorkTiles 12288\n";
    out += melonDS::ComputeRendererShaders::Common;
    out += *spec.source;

    bool ok = transform_common(out, spec.name.c_str());
    replace_all(out, "layout (std430, binding = 1) buffer XSpanSetupsBuffer",
        "layout (std430, set = 0, binding = 2) buffer XSpanSetupsBuffer");
    replace_all(out, "layout (std430, binding = 2) buffer YSpanSetupsBuffer",
        "layout (std430, set = 0, binding = 3) buffer YSpanSetupsBuffer");
    replace_all(out, "layout (std430, binding = 2) buffer ColorTileBuffer",
        "layout (std430, set = 0, binding = 7) buffer ColorTileBuffer");
    replace_all(out, "layout (std430, binding = 3) buffer DepthTileBuffer",
        "layout (std430, set = 0, binding = 8) buffer DepthTileBuffer");
    replace_all(out, "layout (std430, binding = 4) buffer AttrTileBuffer",
        "layout (std430, set = 0, binding = 9) buffer AttrTileBuffer");
    replace_all(out, "layout (std430, binding = 5) buffer ResultBuffer",
        "layout (std430, set = 0, binding = 4) buffer ResultBuffer");

    if (out.find("uimageBuffer SetupIndices") != std::string::npos) {
        ok &= transform_interp(out, spec.name.c_str());
    }
    if (out.find("uniform usampler2DArray CurrentTexture") != std::string::npos) {
        ok &= transform_rasterise(out, spec.name.c_str());
    }
    if (out.find("writeonly uniform image2D FinalFB") != std::string::npos) {
        ok &= transform_final(out, spec.name.c_str());
    }

    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: ds_shader_gen <output-directory> <shader-spec>\n");
        return 2;
    }

    std::vector<ShaderSpec> specs;
    if (!parse_spec_file(argv[2], specs)) {
        return 1;
    }

    const std::filesystem::path out_dir(argv[1]);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[ds-shader-gen] cannot create %s: %s\n", out_dir.string().c_str(),
            ec.message().c_str());
        return 1;
    }

    for (const ShaderSpec& spec : specs) {
        std::string source;
        if (!build_source(spec, source)) {
            return 1;
        }
        const std::filesystem::path path = out_dir / (spec.name + ".comp");
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        if (!file) {
            std::fprintf(stderr, "[ds-shader-gen] cannot open %s\n", path.string().c_str());
            return 1;
        }
        const std::size_t written = std::fwrite(source.data(), 1, source.size(), file);
        std::fclose(file);
        if (written != source.size()) {
            std::fprintf(stderr, "[ds-shader-gen] short write to %s\n", path.string().c_str());
            return 1;
        }
    }
    return 0;
}
