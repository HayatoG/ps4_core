// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: first-run shader compilation (see vk_precompile.h).
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "common/assert.h"
#include "common/assert_scope.h"
#include "common/thread.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_precompile.h"
#include "common/elf_info.h"
#include "core/emulator_settings.h"
#include <fmt/format.h>
#include "common/path_util.h"
#include "video_core/cache_storage.h"
#include "tlg_threads.h"

namespace Vulkan::Precompile {

namespace {

class Reader {
public:
    explicit Reader(std::vector<u8>&& data_) : data{std::move(data_)} {}
    template <typename T>
    bool Get(T& value) {
        if (at + sizeof(T) > data.size()) {
            return false;
        }
        std::memcpy(&value, data.data() + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
    bool Bytes(std::vector<u8>& out, u32 size) {
        if (at + size > data.size()) {
            return false;
        }
        out.assign(data.begin() + at, data.begin() + at + size);
        at += size;
        return true;
    }
    bool Done() const {
        return at == data.size();
    }

private:
    std::vector<u8> data;
    size_t at = 0;
};

template <typename T>
void Put(std::vector<u8>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void PutBytes(std::vector<u8>& out, const void* data, u32 size) {
    Put(out, size);
    const auto* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

/// The code of a stage from its first instruction to the end of its OrbShdr block.
std::span<const u8> CodeWithBinaryInfo(const u32* code) {
    const auto* info = AmdGpu::FindBinaryInfo(code);
    if (!info) {
        return {};
    }
    const auto* begin = reinterpret_cast<const u8*>(code);
    const auto* end = reinterpret_cast<const u8*>(info) + sizeof(AmdGpu::BinaryInfo);
    return {begin, size_t(end - begin)};
}

void PutStage(std::vector<u8>& out, Shader::SwStage sw, const Shader::Info& info,
              const u32* code, const AmdGpu::UserData& user_data,
              const std::optional<Shader::Gcn::FetchShaderData>& fetch) {
    Put(out, u32(sw));
    Put(out, u32(info.hw_stage));
    Put(out, u64(info.pgm_hash));
    const auto bytes = CodeWithBinaryInfo(code);
    PutBytes(out, bytes.data(), u32(bytes.size()));
    for (const u32 word : user_data) {
        Put(out, word);
    }
    Put(out, u32(info.flattened_ud_buf.size()));
    for (const u32 word : info.flattened_ud_buf) {
        Put(out, word);
    }
    if (sw == Shader::SwStage::Vertex && fetch && info.has_fetch_shader) {
        const u32* fetch_code = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
        PutBytes(out, fetch_code, fetch->size);
        Put(out, u32(info.fetch_shader_sgpr_base));
        Put(out, u32(fetch->attributes.size()));
        for (const auto& attribute : fetch->attributes) {
            // The raw V# in guest memory (GetSharp applies the attribute's offset and format).
            const auto sharp =
                info.ReadUdReg<AmdGpu::Buffer>(attribute.sgpr_base, attribute.dword_offset);
            std::array<u32, 4> words;
            std::memcpy(words.data(), &sharp, sizeof(words));
            Put(out, u32(attribute.sgpr_base));
            Put(out, u32(attribute.dword_offset));
            for (const u32 word : words) {
                Put(out, word);
            }
        }
    } else {
        Put(out, u32(0));
        Put(out, u32(0));
        Put(out, u32(0));
    }
}

struct Capture {
    std::mutex mutex;
    std::ofstream out;
    std::unordered_set<u64> seen;
    u32 records = 0;
};

Capture* OpenCapture() {
    static Capture* capture = [] () -> Capture* {
        const char* path = std::getenv("TLG_PRECOMPILE_CAPTURE");
        if (!path || !*path) {
            return nullptr;
        }
        auto* c = new Capture;
        const bool fresh = !std::filesystem::exists(path);
        c->out.open(path, std::ios::binary | std::ios::app);
        if (!c->out) {
            std::printf("Precompile capture: cannot open %s\n", path);
            delete c;
            return nullptr;
        }
        if (fresh) {
            c->out.write(FileMagic, sizeof(FileMagic));
            c->out.write(reinterpret_cast<const char*>(&FileVersion), sizeof(FileVersion));
        }
        std::printf("Precompile capture: appending new pipelines to %s\n", path);
        return c;
    }();
    return capture;
}

void Write(Capture& capture, u64 key, const std::vector<u8>& record) {
    std::scoped_lock lock{capture.mutex};
    if (!capture.seen.insert(key).second) {
        return;
    }
    capture.out.write(reinterpret_cast<const char*>(record.data()), record.size());
    capture.out.flush();
    ++capture.records;
}

void PutRegs(std::vector<u8>& out, const u32* words, size_t count) {
    u32 nonzero = 0;
    for (size_t i = 0; i < count; ++i) {
        nonzero += words[i] != 0;
    }
    Put(out, nonzero);
    for (size_t i = 0; i < count; ++i) {
        if (words[i]) {
            Put(out, u32(i));
            Put(out, words[i]);
        }
    }
}

} // namespace

bool CaptureEnabled() {
    return OpenCapture() != nullptr;
}

void CaptureGraphics(const PipelineSelection& sel) {
    auto* capture = OpenCapture();
    const auto& regs = *sel.regs;
    // Vertex + pixel pipelines only: geometry and tessellation stages need more state.
    if (!capture || regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        return;
    }
    std::vector<u8> record;
    Put(record, u32(0));
    PutRegs(record, regs.reg_array.data(), regs.reg_array.size());
    u32 stages = 0;
    for (const auto* info : sel.infos) {
        stages += info != nullptr;
    }
    Put(record, stages);
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        const auto* info = sel.infos[i];
        if (!info) {
            continue;
        }
        const auto* program = regs.ProgramForStage(u32(info->hw_stage));
        PutStage(record, Shader::SwStage(i), *info, program->Address<const u32*>(),
                 program->user_data, sel.fetch_shader);
    }
    Write(*capture, std::hash<GraphicsPipelineKey>{}(sel.graphics_key), record);
}

void CaptureCompute(const PipelineSelection& sel, const AmdGpu::ComputeProgram& cs) {
    auto* capture = OpenCapture();
    const auto* info = sel.infos[0];
    if (!capture || !info) {
        return;
    }
    std::vector<u8> record;
    Put(record, u32(1));
    PutRegs(record, reinterpret_cast<const u32*>(&cs), sizeof(cs) / sizeof(u32));
    Put(record, u32(1));
    PutStage(record, Shader::SwStage::Compute, *info, cs.Address<const u32*>(), cs.user_data, {});
    // A compute key is the program and its permutation: keep both registers sets apart.
    Write(*capture, info->pgm_hash ^ (u64(info->flattened_ud_buf.size()) << 48) ^
                        std::hash<std::string_view>{}(std::string_view{
                            reinterpret_cast<const char*>(info->flattened_ud_buf.data()),
                            info->flattened_ud_buf.size() * sizeof(u32)}),
          record);
}

std::filesystem::path JobFile() {
    if (const char* path = std::getenv("TLG_PRECOMPILE_JOBS"); path && *path) {
        return path;
    }
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           (std::string{Common::ElfInfo::Instance().GameSerial()} + "-jobs.bin");
}

bool ReadJobs(const std::filesystem::path& path, std::vector<Record>& records) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("Precompile: no job file at %s\n", path.string().c_str());
        return false;
    }
    Reader r{std::vector<u8>{std::istreambuf_iterator<char>(in), {}}};
    std::array<char, 8> magic{};
    u32 version{};
    if (!r.Get(magic) || std::memcmp(magic.data(), FileMagic, sizeof(FileMagic)) ||
        !r.Get(version) || version != FileVersion) {
        std::printf("Precompile: %s is not a version %u job file\n", path.string().c_str(),
                    FileVersion);
        return false;
    }
    while (!r.Done()) {
        Record record{};
        u32 count{};
        bool ok = r.Get(record.kind) && r.Get(count);
        for (u32 i = 0; ok && i < count; ++i) {
            std::pair<u32, u32> reg;
            ok = r.Get(reg.first) && r.Get(reg.second);
            record.regs.push_back(reg);
        }
        ok = ok && r.Get(count);
        for (u32 s = 0; ok && s < count; ++s) {
            Stage stage{};
            u32 size{}, flat{}, sharps{};
            ok = r.Get(stage.sw_stage) && r.Get(stage.hw_stage) && r.Get(stage.hash) &&
                 r.Get(size) && r.Bytes(stage.code, size) && r.Get(stage.user_data) &&
                 r.Get(flat);
            stage.flat.resize(ok ? flat : 0);
            for (u32 i = 0; ok && i < flat; ++i) {
                ok = r.Get(stage.flat[i]);
            }
            ok = ok && r.Get(size) && r.Bytes(stage.fetch_code, size) && r.Get(stage.fetch_sgpr) &&
                 r.Get(sharps);
            for (u32 i = 0; ok && i < sharps; ++i) {
                VertexSharp vs{};
                ok = r.Get(vs.sgpr_base) && r.Get(vs.dword_offset) && r.Get(vs.sharp);
                stage.vertex_sharps.push_back(vs);
            }
            record.stages.push_back(std::move(stage));
        }
        if (!ok) {
            std::printf("Precompile: %s is truncated after %zu records\n",
                        path.string().c_str(), records.size());
            break;
        }
        records.push_back(std::move(record));
    }
    return !records.empty();
}

} // namespace Vulkan::Precompile

namespace Vulkan {

namespace {

/// Host memory standing in for the guest memory a draw's registers point at: shader code,
/// fetch shaders and vertex buffer descriptor tables. Shader addresses are 40-bit fields in
/// units of 256 bytes, so every block is 256-aligned and below 2^48.
class Arena {
public:
    void* Place(const void* data, size_t size) {
        auto& block = blocks.emplace_back(std::make_unique<u8[]>(size + 512));
        auto* aligned =
            reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(block.get()) + 255) & ~uintptr_t{255});
        std::memset(aligned, 0, size + 256);
        if (data) {
            std::memcpy(aligned, data, size);
        }
        ASSERT_MSG(reinterpret_cast<uintptr_t>(aligned) < (uintptr_t{1} << 48),
                   "precompile arena above the 48-bit shader address range");
        return aligned;
    }

private:
    std::vector<std::unique_ptr<u8[]>> blocks;
};

void SetPointer(AmdGpu::UserData& user_data, u32 sgpr, const void* pointer) {
    const auto value = reinterpret_cast<uintptr_t>(pointer);
    user_data[sgpr] = u32(value);
    user_data[sgpr + 1] = u32(value >> 32);
}

/// The user data of a stage with its code pointers moved into the arena.
void PlaceStage(Arena& arena, const Precompile::Stage& stage, u64& address,
                AmdGpu::UserData& user_data) {
    void* code = arena.Place(stage.code.data(), stage.code.size());
    address = reinterpret_cast<uintptr_t>(code) >> 8;
    std::copy(stage.user_data.begin(), stage.user_data.end(), user_data.begin());
    if (!stage.fetch_code.empty() && stage.fetch_sgpr + 1 < user_data.size()) {
        SetPointer(user_data, stage.fetch_sgpr,
                   arena.Place(stage.fetch_code.data(), stage.fetch_code.size()));
    }
    // One table per user data register that points at vertex buffer descriptors.
    std::unordered_map<u32, u32> table_dwords;
    for (const auto& vs : stage.vertex_sharps) {
        auto& dwords = table_dwords[vs.sgpr_base];
        dwords = std::max(dwords, vs.dword_offset + 4);
    }
    for (const auto& [sgpr, dwords] : table_dwords) {
        auto* table = static_cast<u32*>(arena.Place(nullptr, dwords * sizeof(u32)));
        for (const auto& vs : stage.vertex_sharps) {
            if (vs.sgpr_base == sgpr) {
                std::memcpy(table + vs.dword_offset, vs.sharp.data(), sizeof(vs.sharp));
            }
        }
        if (sgpr + 1 < user_data.size()) {
            SetPointer(user_data, sgpr, table);
        }
    }
}

} // namespace

namespace {
std::string ReadSmallFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string{std::istreambuf_iterator<char>(in), {}};
}
void WriteSmallFile(const std::filesystem::path& path, const std::string& text) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << text;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
}
} // namespace

bool PipelineCache::PrecompileFirstStart(const std::function<void(u32 total)>& start,
                                         const std::function<void(u32 done, u32 total)>& progress) {
    const auto jobs = Precompile::JobFile();
    std::error_code ec;
    if (!std::filesystem::exists(jobs, ec) || !EmulatorSettings.IsPipelineCacheEnabled() ||
        EmulatorSettings.IsPipelineCacheArchived()) {
        return false;
    }
    CheckCacheProfile(); // opens the cache; a rebuilt one has lost its done file too
    const auto folder = Storage::GameCacheDir();
    const auto size = std::filesystem::file_size(jobs, ec);
    const auto stamp = std::filesystem::last_write_time(jobs, ec).time_since_epoch().count();
    // The driver's own pipeline cache is lost when the driver or the GPU changes: then compile
    // again so that the pipelines are in it (SCSKiller's finding: a driver update clears it).
    const auto uuid = instance.GetPipelineCacheUUID();
    u64 uuid_hash = 0;
    for (const u8 byte : uuid) {
        uuid_hash = uuid_hash * 1099511628211ull ^ byte;
    }
    const std::string fingerprint =
        fmt::format("{} {} {} {:x}:{:x}:{:x}:{:x}\n", size, stamp, ShaderBinaryVersionForCache(),
                    instance.GetVendorID(), instance.GetDeviceID(), instance.GetDriverVersion(),
                    uuid_hash);
    if (ReadSmallFile(folder / Precompile::DoneFile) == fingerprint) {
        return false;
    }
    std::vector<Precompile::Record> records;
    if (!Precompile::ReadJobs(jobs, records)) {
        return false;
    }
    // A record that crashed the last start is skipped (it compiles in game, as before).
    // "<record>|<crashes>|<fingerprint>": after three starts that stopped while compiling, it
    // gives up (the game compiles in game as before) instead of failing at every start.
    size_t first = 0;
    u32 crashes = 0;
    if (const auto text = ReadSmallFile(folder / Precompile::ProgressFile); !text.empty()) {
        const auto split = text.find('|');
        const auto split2 = split == std::string::npos ? split : text.find('|', split + 1);
        if (split2 != std::string::npos && text.substr(split2 + 1) == fingerprint) {
            first = std::strtoull(text.c_str(), nullptr, 10) + 1;
            crashes = u32(std::strtoul(text.c_str() + split + 1, nullptr, 10)) + 1;
            std::printf("Precompile: start %u stopped in record %zu: resuming after it\n", crashes,
                        first - 1);
        }
    }
    if (crashes >= 3) {
        std::printf("Precompile: stopped %u times, giving up: shaders compile in game\n", crashes);
        WriteSmallFile(folder / Precompile::DoneFile, fingerprint);
        std::error_code remove_ec;
        std::filesystem::remove(folder / Precompile::ProgressFile, remove_ec);
        return false;
    }
    if (first >= records.size()) {
        WriteSmallFile(folder / Precompile::DoneFile, fingerprint);
        return false;
    }
    records.erase(records.begin(), records.begin() + first);
    start(u32(records.size()));
    const auto progress_path = folder / Precompile::ProgressFile;
    Precompile(records, [&](u32 done, u32 total) {
        if (done < total) {
            WriteSmallFile(progress_path, fmt::format("{}|{}|{}", first + done, crashes, fingerprint));
        }
        progress(done, total);
    });
    // Release what the compilation loaded: the game loads each program again when it first uses
    // it (LoadProgramFromCache), so this start does not keep the whole game's shaders in memory.
    Storage::DataBase::Instance().WaitIdle();
    size_t released = 0;
    {
        std::unique_lock lock{programs_mutex};
        for (auto& [hash, program] : program_cache) {
            auto& next = next_permutation[hash];
            next = std::max(next, program->modules.size());
            for (auto& module : program->modules) {
                if (module.module) {
                    instance.GetDevice().destroyShaderModule(module.module);
                    ++released;
                }
            }
        }
        program_cache.clear();
    }
    std::printf("Precompile: released %zu shader modules (they load again on first use)\n",
                released);
    WriteSmallFile(folder / Precompile::DoneFile, fingerprint);
    std::filesystem::remove(progress_path, ec);
    return true;
}

namespace {
/// One pipeline for the build threads: copies of what the translator chose for a record, and the
/// record's guest code (the vertex fetch shader is read again while the pipeline is built).
struct BuildJob {
    u32 record = 0;
    bool compute = false;
    std::unique_ptr<Arena> arena;
    std::unordered_map<u64, std::vector<u32>> flats;
    PipelineSelection selection{};
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    ComputePipelineKey compute_key{};
    u64 graphics_hash = 0;
    GraphicsPipeline::SerializationSupport graphics_data{};
    ComputePipeline::SerializationSupport compute_data{};
    std::string error; ///< empty: built
};

/// The threads that build pipelines while the GPU thread translates the next records: building
/// a pipeline is the driver compiling it, most of the first start's time, and it runs on every
/// core. At most `limit` jobs wait or run, so their copies stay small.
class BuildPool {
public:
    BuildPool(unsigned count, size_t limit_, std::function<void(BuildJob&, unsigned)> build_)
        : limit{limit_}, build{std::move(build_)} {
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Work(i); });
        }
    }
    ~BuildPool() {
        Finish();
    }

    void Submit(std::unique_ptr<BuildJob> job) {
        std::unique_lock lock{mutex};
        space.wait(lock, [&] { return queued.size() + running < limit; });
        queued.push_back(std::move(job));
        work.notify_one();
    }
    std::vector<std::unique_ptr<BuildJob>> TakeDone() {
        std::scoped_lock lock{mutex};
        return std::exchange(done, {});
    }
    void Finish() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        work.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    void Work(unsigned index) {
        Common::SetCurrentThreadName(("tlg:Precompile" + std::to_string(index)).c_str());
        for (;;) {
            std::unique_ptr<BuildJob> job;
            {
                std::unique_lock lock{mutex};
                work.wait(lock, [&] { return stopping || !queued.empty(); });
                if (queued.empty()) {
                    return;
                }
                job = std::move(queued.front());
                queued.pop_front();
                ++running;
            }
            build(*job, index);
            {
                std::scoped_lock lock{mutex};
                --running;
                done.push_back(std::move(job));
            }
            space.notify_one();
        }
    }

    const size_t limit;
    std::function<void(BuildJob&, unsigned)> build;
    std::mutex mutex;
    std::condition_variable work, space;
    std::deque<std::unique_ptr<BuildJob>> queued;
    std::vector<std::unique_ptr<BuildJob>> done;
    size_t running = 0;
    bool stopping = false;
    std::vector<std::thread> threads;
};

/// Build threads: TLG_PRECOMPILE_THREADS (ISS_PRECOMPILE_THREADS), else every hardware thread but
/// the GPU thread's. 0 builds on the GPU thread, as before.
unsigned BuildThreads() {
    for (const char* name : {"ISS_PRECOMPILE_THREADS", "TLG_PRECOMPILE_THREADS"}) {
        if (const char* value = std::getenv(name); value && *value) {
            return u32(std::strtoul(value, nullptr, 10));
        }
    }
    return std::max(1u, TlgThreads::Available() - 1);
}
} // namespace

Precompile::Result PipelineCache::Precompile(
    const std::vector<Precompile::Record>& records,
    const std::function<void(u32 done, u32 total)>& progress) {
    Precompile::Result result{};
    // Opens the shader cache (rebuilt when the profile changed) and reads its indices, so the
    // shaders and pipelines built here are saved like the game's own.
    CheckCacheProfile();
    auto regs = std::make_unique<AmdGpu::Regs>();
    AmdGpu::ComputeProgram cs{};
    // Each pipeline is built (the driver caches it), saved, and released: the game builds it again
    // when it needs it. Keeping all of them took ~7 GiB with the whole game's job file.
    std::unordered_set<u64> built_graphics, built_compute;
    const auto started = std::chrono::steady_clock::now();

    // Translation (GCN to SPIR-V, the shader cache) stays on this thread, in record order; the
    // driver builds the pipelines on the build threads, each into its own pipeline cache.
    const unsigned threads = BuildThreads();
    std::vector<vk::UniquePipelineCache> caches;
    for (unsigned i = 0; i < threads; ++i) {
        auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
        if (cache_result != vk::Result::eSuccess) {
            break;
        }
        if (instance.GetDevice().mergePipelineCaches(*cache, *pipeline_cache) != vk::Result::eSuccess) {
            break;
        }
        caches.push_back(std::move(cache));
    }
    const auto build = [this, &caches](BuildJob& job, unsigned index) {
        Shader::Info::replay_flats = &job.flats;
        try {
            const Common::ScopedAssertThrows recoverable;
            auto& cache = caches.empty() ? *pipeline_cache : *caches[index];
            auto& s = job.selection;
            if (job.compute) {
                ComputePipeline pipeline{instance, scheduler, desc_heap, profile, cache,
                                         job.compute_key, *s.infos[0], s.modules[0],
                                         job.compute_data, false, nullptr};
            } else {
                GraphicsPipeline pipeline{instance, scheduler, desc_heap, profile, s.graphics_key,
                                          cache, s.infos, s.runtime_infos, s.fetch_shader,
                                          s.modules, job.graphics_data, false};
            }
        } catch (const std::exception& e) {
            job.error = e.what();
            if (job.error.empty()) {
                job.error = "unknown error";
            }
        }
        Shader::Info::replay_flats = nullptr;
    };
    std::optional<BuildPool> pool;
    if (!caches.empty()) {
        pool.emplace(u32(caches.size()), caches.size() * 4, build);
    }
    std::printf("Precompile: building pipelines on %zu threads\n", caches.size());
    std::set<u32> in_flight; // records whose pipeline is being built
    const auto collect = [&](std::vector<std::unique_ptr<BuildJob>> jobs) {
        for (auto& job : jobs) {
            in_flight.erase(job->record);
            if (!job->error.empty()) {
                if (result.failed < 20) {
                    std::printf("Precompile: record %u failed: %s\n", job->record + 1,
                                job->error.c_str());
                }
                ++result.failed;
            } else if (job->compute) {
                RegisterPipelineData(job->compute_key, job->compute_data);
                ++num_new_pipelines;
                ++result.compute;
            } else {
                RegisterPipelineData(job->selection.graphics_key, job->graphics_hash,
                                     job->graphics_data);
                ++num_new_pipelines;
                ++result.graphics;
            }
        }
    };
    /// The build job of a translated selection: its own copies of the stage infos and user data.
    const auto make_job = [](u32 record, PipelineSelection& selection,
                             std::unique_ptr<Arena> arena,
                             std::unordered_map<u64, std::vector<u32>>& flats) {
        auto job = std::make_unique<BuildJob>();
        job->record = record;
        job->arena = std::move(arena);
        job->flats = std::move(flats);
        job->selection = selection;
        job->selection.regs = nullptr;
        job->selection.cs_regs = nullptr;
        for (u32 i = 0; i < MaxShaderStages; ++i) {
            if (selection.infos[i]) {
                job->infos[i] = std::make_unique<Shader::Info>(*selection.infos[i]);
                job->user_data[i].assign(selection.infos[i]->user_data.begin(),
                                         selection.infos[i]->user_data.end());
                job->infos[i]->user_data = job->user_data[i];
                job->selection.infos[i] = job->infos[i].get();
            }
        }
        return job;
    };

    for (const auto& record : records) {
        const u32 index = result.records++;
        if (pool) {
            collect(pool->TakeDone());
        }
        if (progress) {
            progress(in_flight.empty() ? index : std::min(index, *in_flight.begin()),
                     u32(records.size()));
        }
        auto arena = std::make_unique<Arena>();
        std::unordered_map<u64, std::vector<u32>> flats;
        for (const auto& stage : record.stages) {
            flats[stage.hash] = stage.flat;
        }
        Shader::Info::replay_flats = &flats;
        PipelineSelection selection{};
        selection.sync_compile = true;
        std::unique_ptr<BuildJob> job;
        try {
            const Common::ScopedAssertThrows recoverable;
            if (record.kind == 0) {
                regs->reg_array.fill(0);
                for (const auto& [reg, value] : record.regs) {
                    if (reg < regs->reg_array.size()) {
                        regs->reg_array[reg] = value;
                    }
                }
                for (const auto& stage : record.stages) {
                    auto* program =
                        const_cast<AmdGpu::ShaderProgram*>(regs->ProgramForStage(stage.hw_stage));
                    u64 address{};
                    PlaceStage(*arena, stage, address, program->user_data);
                    program->address = address;
                }
                selection.regs = regs.get();
                if (!RefreshGraphicsKey(selection)) {
                    ++result.failed;
                } else if (graphics_pipelines.contains(selection.graphics_key) ||
                           !built_graphics.insert(std::hash<GraphicsPipelineKey>{}(
                                selection.graphics_key)).second) {
                    ++result.skipped;
                } else {
                    job = make_job(index, selection, std::move(arena), flats);
                    job->graphics_hash = std::hash<GraphicsPipelineKey>{}(selection.graphics_key);
                }
            } else {
                auto* words = reinterpret_cast<u32*>(&cs);
                std::memset(&cs, 0, sizeof(cs));
                for (const auto& [reg, value] : record.regs) {
                    if (reg < sizeof(cs) / sizeof(u32)) {
                        words[reg] = value;
                    }
                }
                if (record.stages.size() == 1) {
                    u64 address{};
                    PlaceStage(*arena, record.stages[0], address, cs.user_data);
                    cs.address = address;
                }
                selection.regs = sel.regs;
                selection.cs_regs = &cs;
                ComputePipelineKey key{};
                if (!RefreshComputeKey(selection, key)) {
                    ++result.failed;
                } else if (compute_pipelines.contains(key) ||
                           !built_compute.insert(std::hash<ComputePipelineKey>{}(key)).second) {
                    ++result.skipped;
                } else {
                    job = make_job(index, selection, std::move(arena), flats);
                    job->compute = true;
                    job->compute_key = key;
                }
            }
        } catch (const std::exception& e) {
            if (result.failed < 20) {
                std::printf("Precompile: record %u failed: %s\n", result.records, e.what());
            }
            ++result.failed;
        }
        Shader::Info::replay_flats = nullptr;
        if (!job) {
            continue;
        }
        if (pool) {
            in_flight.insert(index);
            pool->Submit(std::move(job));
        } else {
            build(*job, 0);
            std::vector<std::unique_ptr<BuildJob>> one;
            one.push_back(std::move(job));
            collect(std::move(one));
        }
    }
    if (pool) {
        pool->Finish();
        collect(pool->TakeDone());
        pool.reset();
        // The pipelines the build threads compiled go into the cache the game saves.
        std::vector<vk::PipelineCache> sources;
        for (const auto& cache : caches) {
            sources.push_back(*cache);
        }
        if (instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources) !=
            vk::Result::eSuccess) {
            std::printf("Precompile: merging the build threads' pipeline caches failed\n");
        }
    }
    if (progress) {
        progress(u32(records.size()), u32(records.size()));
    }
    std::printf("Precompile: %u records in %.1f s: %u graphics and %u compute pipelines built, "
                "%u already there, %u failed\n",
                result.records,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                result.graphics, result.compute, result.skipped, result.failed);
    return result;
}

} // namespace Vulkan
