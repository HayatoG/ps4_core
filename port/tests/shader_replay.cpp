// SPDX-License-Identifier: GPL-2.0-or-later
// Shader replay (first-run shader compilation, gate G4.1): translates every shader of a shader
// cache again, with no guest memory, from what its .meta recorded (flattened user data, runtime
// info, first bindings, fetch shader), and compares the SPIR-V with the cached .spv. It shows
// whether the precompiler can produce, from data alone, what the game compiles while playing.
//   out/gpu/shader-replay <cache dir> <codes.bin> [--user dir] [--limit N] [--diff dir]
// --user: the player folder whose tlg.ini (backports) the game ran with; it changes the version.
// codes.bin (made locally from the player's game files, never shipped): repeated records of
// u64 shader hash, u32 code size in bytes, then the code. The profile comes from profile.bin.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "tlggpu.h"

namespace fs = std::filesystem;
using namespace Shader;

static std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(in), {}};
}

// A V# whose fields give the specialization's number class and swizzle. The exact number
// format is not in the .meta (only its class after RemapNumberFormat): --search-formats 1 tries
// the integer-class formats that convert differently (SnormNz, Sscaled, Uscaled, Unorm 32).
// Debugging: REPLAY_HASH=<hex> prints that shader's fetch attributes, REPLAY_ATTR=i:nfmt:dfmt
// forces one attribute's V#.
static AmdGpu::Buffer VertexSharp(const VsAttribSpecialization& spec) {
    AmdGpu::Buffer buffer{};
    if (spec == VsAttribSpecialization{}) {
        return buffer; // no buffer bound (num_records 0): the specialization keeps the default
    }
    buffer.num_records = 1;
    switch (spec.num_class) {
    case AmdGpu::NumberClass::Sint:
        buffer.num_format = static_cast<u32>(AmdGpu::NumberFormat::Sint);
        break;
    case AmdGpu::NumberClass::Uint:
        buffer.num_format = static_cast<u32>(AmdGpu::NumberFormat::Uint);
        break;
    default:
        buffer.num_format = static_cast<u32>(AmdGpu::NumberFormat::Float);
        break;
    }
    buffer.data_format = static_cast<u32>(AmdGpu::DataFormat::Format32_32_32_32);
    buffer.dst_sel_x = static_cast<u32>(spec.dst_select.r);
    buffer.dst_sel_y = static_cast<u32>(spec.dst_select.g);
    buffer.dst_sel_z = static_cast<u32>(spec.dst_select.b);
    buffer.dst_sel_w = static_cast<u32>(spec.dst_select.a);
    return buffer;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <cache dir> <codes.bin> [--limit N] [--diff dir]\n",
                     argv[0]);
        return 2;
    }
    const fs::path cache{argv[1]};
    size_t limit = SIZE_MAX;
    std::optional<fs::path> diff_dir;
    const char* user_dir = nullptr;
    bool search_formats = false;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--search-formats")) {
            search_formats = argv[i + 1][0] == '1';
        } else if (!std::strcmp(argv[i], "--user")) {
            user_dir = argv[i + 1];
        } else if (!std::strcmp(argv[i], "--limit")) {
            limit = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--diff")) {
            diff_dir = argv[i + 1];
            fs::create_directories(*diff_dir);
        }
    }

    if (user_dir) {
        fs::current_path(user_dir); // tlg.ini is read from the current folder
    }
    const TlgGpuConfig config{.title = "inFAMOUS Second Son", .serial = "CUSA00223",
                              .user_dir = ".", .sdk_version = 0, .psf_attributes = 0,
                              .width = 0, .height = 0};
    tlggpu_init_headless(&config);
    std::printf("cache binary version %u\n", Vulkan::ShaderBinaryVersionForCache());

    Profile profile{};
    {
        const auto data = ReadFile(cache / "profile.bin");
        if (data.size() != sizeof(profile)) {
            std::fprintf(stderr, "profile.bin: %zu bytes, expected %zu (another build?)\n",
                         data.size(), sizeof(profile));
            return 1;
        }
        std::memcpy(&profile, data.data(), sizeof(profile));
    }

    std::unordered_map<u64, std::vector<u32>> codes;
    {
        const auto data = ReadFile(argv[2]);
        for (size_t at = 0; at + 12 <= data.size();) {
            u64 hash;
            u32 size;
            std::memcpy(&hash, &data[at], 8);
            std::memcpy(&size, &data[at + 8], 4);
            at += 12;
            if (at + size > data.size() || size % 4) {
                std::fprintf(stderr, "codes.bin: truncated record\n");
                return 1;
            }
            auto& code = codes[hash];
            code.resize(size / 4);
            std::memcpy(code.data(), &data[at], size);
            at += size;
        }
    }

    struct Tally {
        u32 total = 0, identical = 0, different = 0, spec_different = 0, no_code = 0,
            unreadable = 0, failed = 0, no_spv = 0, different_same_spec = 0, found_by_search = 0;
    };
    std::map<std::string, Tally> by_stage;
    std::map<std::string, u32> spec_reasons;
    Tally all;
    std::vector<fs::path> metas;
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().extension() == ".meta") {
            metas.push_back(entry.path());
        }
    }
    std::sort(metas.begin(), metas.end());
    Pools pools;
    std::unordered_map<u64, std::vector<u32>> replay;
    size_t done = 0;
    for (const auto& path : metas) {
        if (done++ >= limit) {
            break;
        }
        Serialization::Archive ar{ReadFile(path)};
        Info meta_info{};
        StageSpecialization spec{};
        spec.info = &meta_info;
        std::optional<Gcn::FetchShaderData> fetch;
        size_t perm_idx{};
        ++all.total;
        if (!Vulkan::LoadShaderMeta(ar, meta_info, fetch, spec, perm_idx)) {
            ++all.unreadable;
            continue;
        }
        const std::string stage = fmt::format("{}", meta_info.hw_stage);
        auto& tally = by_stage[stage];
        ++tally.total;
        const auto record = [&](u32 Tally::* field) {
            ++(tally.*field);
            ++(all.*field);
        };
        const auto code = codes.find(meta_info.pgm_hash);
        if (code == codes.end()) {
            record(&Tally::no_code);
            continue;
        }
        const auto expected_bytes =
            ReadFile(cache / fmt::format("{:#018x}_{}.spv", meta_info.pgm_hash, perm_idx));
        if (expected_bytes.empty()) {
            record(&Tally::no_spv);
            continue;
        }

        std::array<u32, ShaderParams::NumShaderUserData> user_data{};
        std::memcpy(user_data.data(), meta_info.flattened_ud_buf.data(),
                    std::min(meta_info.flattened_ud_buf.size(), user_data.size()) * sizeof(u32));
        const ShaderParams params{.user_data = user_data, .code = code->second,
                                  .hash = meta_info.pgm_hash};
        Info info{meta_info.hw_stage, meta_info.sw_stage, params};

        Gcn::FetchShaderSnapshot snapshot{};
        snapshot.info = &info;
        snapshot.fetch = spec.fetch_shader_data;
        if (snapshot.fetch) {
            for (size_t i = 0; i < snapshot.fetch->attributes.size(); ++i) {
                const auto& attribute = snapshot.fetch->attributes[i];
                auto buffer = i < spec.vs_attribs.size() ? VertexSharp(spec.vs_attribs[i])
                                                         : AmdGpu::Buffer{};
                // A typed fetch load names its own format, as VertexAttribute::GetSharp applies.
                if (attribute.data_format && buffer) {
                    buffer.data_format = attribute.data_format;
                    buffer.num_format = attribute.num_format;
                }
                if (const char* force = std::getenv("REPLAY_ATTR")) { // "index:nfmt:dfmt"
                    unsigned at, nfmt, dfmt;
                    if (std::sscanf(force, "%u:%u:%u", &at, &nfmt, &dfmt) == 3 && at == i) {
                        buffer.num_format = nfmt;
                        buffer.data_format = dfmt;
                    }
                }
                snapshot.buffers.push_back(buffer);
            }
        }
        if (const char* only = std::getenv("REPLAY_HASH");
            only && std::strtoull(only, nullptr, 16) == meta_info.pgm_hash && snapshot.fetch) {
            for (size_t i = 0; i < snapshot.fetch->attributes.size(); ++i) {
                const auto& a = snapshot.fetch->attributes[i];
                const auto& v = spec.vs_attribs[i];
                std::printf("  attr %zu: semantic %u vgpr %u n %u sgpr %u dw %u inst %u ioff %u "
                            "dfmt %u nfmt %u | class %u dst %u%u%u%u divisor %u\n",
                            i, a.semantic, a.dest_vgpr, a.num_elements, a.sgpr_base,
                            a.dword_offset, a.instance_data, a.inst_offset, a.data_format,
                            a.num_format, u32(v.num_class), u32(v.dst_select.r),
                            u32(v.dst_select.g), u32(v.dst_select.b), u32(v.dst_select.a),
                            v.divisor);
            }
        }
        replay = {{meta_info.pgm_hash, meta_info.flattened_ud_buf}};
        Info::replay_flats = &replay;
        Gcn::ScopedFetchShaderSnapshot scoped{snapshot};

        std::vector<u32> spv;
        bool spec_same = true;
        try {
            RuntimeInfo runtime = spec.runtime_info;
            Backend::Bindings binding = spec.start;
            pools.ReleaseContents();
            auto program = TranslateProgram(code->second, pools, info, runtime, profile);
            spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
            // The same specialization the game would look up with (the original runtime info).
            const StageSpecialization again{info, spec.runtime_info, profile, spec.start};
            spec_same = again == spec;
            if (!spec_same) {
                record(&Tally::spec_different);
                std::string what;
                const auto note = [&](bool differs, const char* name) {
                    if (differs) {
                        what += what.empty() ? name : std::string{", "} + name;
                    }
                };
                note(again.vs_attribs != spec.vs_attribs, "vs_attribs");
                note(again.fetch_shader_data != spec.fetch_shader_data, "fetch");
                note(again.buffers != spec.buffers, "buffers");
                note(again.images != spec.images, "images");
                note(again.samplers != spec.samplers, "samplers");
                note(again.fmasks != spec.fmasks, "fmasks");
                note(again.bitset != spec.bitset, "bitset");
                note(!(again.runtime_info == spec.runtime_info), "runtime_info");
                note(again.start != spec.start, "start");
                ++spec_reasons[stage + ": " + what];
            }
        } catch (const std::exception& e) {
            std::printf("FAILED %s: %s\n", path.filename().string().c_str(), e.what());
            record(&Tally::failed);
            Info::replay_flats = nullptr;
            continue;
        }
        Info::replay_flats = nullptr;

        const auto matches = [&](const std::vector<u32>& words) {
            return words.size() * sizeof(u32) == expected_bytes.size() &&
                   !std::memcmp(words.data(), expected_bytes.data(), expected_bytes.size());
        };
        bool same = matches(spv);
        if (!same && search_formats && snapshot.fetch && spec_same) {
            // The .meta keeps a vertex attribute's number class (after RemapNumberFormat), not
            // its format: try the formats of each integer class that convert differently.
            using NF = AmdGpu::NumberFormat;
            using DF = AmdGpu::DataFormat;
            static constexpr std::array<std::pair<NF, DF>, 4> sint_formats{
                {{NF::Sint, DF::Format32_32_32_32}, {NF::Sscaled, DF::Format8_8_8_8},
                 {NF::SnormNz, DF::Format8_8_8_8}, {NF::SnormNz, DF::Format16_16_16_16}}};
            static constexpr std::array<std::pair<NF, DF>, 4> uint_formats{
                {{NF::Uint, DF::Format32_32_32_32}, {NF::Uscaled, DF::Format8_8_8_8},
                 {NF::Unorm, DF::Format32_32_32_32}, {NF::Uint, DF::Format32_32_32_32}}};
            std::vector<size_t> ints;
            for (size_t i = 0; i < snapshot.buffers.size(); ++i) {
                if (snapshot.buffers[i] && i < spec.vs_attribs.size() &&
                    spec.vs_attribs[i].num_class != AmdGpu::NumberClass::Float) {
                    ints.push_back(i);
                }
            }
            if (!ints.empty() && ints.size() <= 5) {
                size_t combos = 1;
                for (size_t k = 0; k < ints.size(); ++k) {
                    combos *= 4;
                }
                for (size_t c = 1; c < combos && !same; ++c) {
                    size_t rest = c;
                    for (const size_t i : ints) {
                        const auto& table = spec.vs_attribs[i].num_class == AmdGpu::NumberClass::Sint
                                                ? sint_formats
                                                : uint_formats;
                        const auto [nfmt, dfmt] = table[rest % 4];
                        rest /= 4;
                        snapshot.buffers[i].num_format = static_cast<u32>(nfmt);
                        snapshot.buffers[i].data_format = static_cast<u32>(dfmt);
                    }
                    try {
                        Info retry{meta_info.hw_stage, meta_info.sw_stage, params};
                        snapshot.info = &retry;
                        replay = {{meta_info.pgm_hash, meta_info.flattened_ud_buf}};
                        Info::replay_flats = &replay;
                        RuntimeInfo runtime = spec.runtime_info;
                        Backend::Bindings binding = spec.start;
                        pools.ReleaseContents();
                        auto program = TranslateProgram(code->second, pools, retry, runtime, profile);
                        same = matches(Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding));
                    } catch (const std::exception& e) {
                        std::printf("  search %zu failed: %s\n", c, e.what());
                    }
                    Info::replay_flats = nullptr;
                }
                if (same) {
                    record(&Tally::found_by_search);
                }
            }
        }
        if (same) {
            record(&Tally::identical);
        } else {
            record(&Tally::different);
            if (spec_same) {
                record(&Tally::different_same_spec);
            }
            if (diff_dir) {
                const auto name = fmt::format("{:#018x}_{}", meta_info.pgm_hash, perm_idx);
                std::ofstream(*diff_dir / (name + ".replay.spv"), std::ios::binary)
                    .write(reinterpret_cast<const char*>(spv.data()), spv.size() * sizeof(u32));
                std::ofstream(*diff_dir / (name + ".cache.spv"), std::ios::binary)
                    .write(reinterpret_cast<const char*>(expected_bytes.data()),
                           expected_bytes.size());
            }
        }
    }

    const auto line = [](const char* name, const Tally& t) {
        std::printf("%-8s %5u shaders: %5u identical SPIR-V, %5u different, %u spec different, "
                    "%u no code, %u no .spv, %u failed, %u unreadable; different with the same spec %u; identical after the format search %u\n",
                    name, t.total, t.identical, t.different, t.spec_different, t.no_code,
                    t.no_spv, t.failed, t.unreadable, t.different_same_spec, t.found_by_search);
    };
    for (const auto& [stage, tally] : by_stage) {
        line(stage.c_str(), tally);
    }
    line("all", all);
    for (const auto& [reason, count] : spec_reasons) {
        std::printf("spec differs in %s: %u\n", reason.c_str(), count);
    }
    return 0;
}
