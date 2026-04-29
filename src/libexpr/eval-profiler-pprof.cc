#include "nix/expr/eval-profiler.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/counter.hh"
#include "nix/util/lru-cache.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#if NIX_USE_BOEHMGC
#  include <gc/gc_mark.h>
#endif

namespace nix {

namespace {

// Minimal protobuf encoder — just enough to emit the pprof Profile message.
// All pprof field numbers and wire types are hardcoded to avoid a protobuf dependency.
class ProtobufEncoder
{
    std::string buf;

    void writeVarint(uint64_t v)
    {
        while (v >= 0x80) {
            buf.push_back(static_cast<char>(v | 0x80));
            v >>= 7;
        }
        buf.push_back(static_cast<char>(v));
    }

    void writeTag(uint32_t fieldNumber, uint32_t wireType)
    {
        writeVarint((static_cast<uint64_t>(fieldNumber) << 3) | wireType);
    }

    void writeVarintField(uint32_t fieldNumber, uint64_t value)
    {
        if (value == 0)
            return;
        writeTag(fieldNumber, 0);
        writeVarint(value);
    }

    void writeSignedVarintField(uint32_t fieldNumber, int64_t value)
    {
        if (value == 0)
            return;
        writeTag(fieldNumber, 0);
        writeVarint(static_cast<uint64_t>(value));
    }

    void writeBytesField(uint32_t fieldNumber, std::string_view data)
    {
        writeTag(fieldNumber, 2);
        writeVarint(data.size());
        buf.append(data);
    }

    void writeMessageField(uint32_t fieldNumber, const ProtobufEncoder & nested)
    {
        writeBytesField(fieldNumber, nested.data());
    }

    void writePackedInt64Field(uint32_t fieldNumber, const std::vector<int64_t> & values)
    {
        if (values.empty())
            return;
        ProtobufEncoder packed;
        for (auto v : values)
            packed.writeVarint(static_cast<uint64_t>(v));
        writeBytesField(fieldNumber, packed.data());
    }

    void writePackedUint64Field(uint32_t fieldNumber, const std::vector<uint64_t> & values)
    {
        if (values.empty())
            return;
        ProtobufEncoder packed;
        for (auto v : values)
            packed.writeVarint(v);
        writeBytesField(fieldNumber, packed.data());
    }

public:
    std::string_view data() const { return {buf.data(), buf.size()}; }
    const std::string & str() const { return buf; }
    void clear() { buf.clear(); }

    // pprof ValueType: { int64 type = 1; int64 unit = 2; }
    static ProtobufEncoder valueType(int64_t typeIdx, int64_t unitIdx)
    {
        ProtobufEncoder e;
        e.writeSignedVarintField(1, typeIdx);
        e.writeSignedVarintField(2, unitIdx);
        return e;
    }

    // pprof Sample: { repeated uint64 location_id = 1; repeated int64 value = 2; }
    static ProtobufEncoder sample(const std::vector<uint64_t> & locationIds, const std::vector<int64_t> & values)
    {
        ProtobufEncoder e;
        e.writePackedUint64Field(1, locationIds);
        e.writePackedInt64Field(2, values);
        return e;
    }

    // pprof Location: { uint64 id = 1; repeated Line line = 4; }
    static ProtobufEncoder location(uint64_t id, const std::vector<ProtobufEncoder> & lines)
    {
        ProtobufEncoder e;
        e.writeVarintField(1, id);
        for (auto & line : lines)
            e.writeMessageField(4, line);
        return e;
    }

    // pprof Line: { uint64 function_id = 1; int64 line = 2; }
    static ProtobufEncoder line(uint64_t functionId, int64_t lineNumber)
    {
        ProtobufEncoder e;
        e.writeVarintField(1, functionId);
        e.writeSignedVarintField(2, lineNumber);
        return e;
    }

    // pprof Function: { uint64 id = 1; int64 name = 2; int64 system_name = 3; int64 filename = 4; int64 start_line = 5; }
    static ProtobufEncoder function(uint64_t id, int64_t nameIdx, int64_t sysNameIdx, int64_t filenameIdx, int64_t startLine)
    {
        ProtobufEncoder e;
        e.writeVarintField(1, id);
        e.writeSignedVarintField(2, nameIdx);
        e.writeSignedVarintField(3, sysNameIdx);
        e.writeSignedVarintField(4, filenameIdx);
        e.writeSignedVarintField(5, startLine);
        return e;
    }

    // Build the top-level Profile message
    void writeProfile(
        const std::vector<ProtobufEncoder> & sampleTypes,    // field 1
        const std::vector<ProtobufEncoder> & samples,         // field 2
        const std::vector<ProtobufEncoder> & locations,       // field 4
        const std::vector<ProtobufEncoder> & functions,       // field 5
        const std::vector<std::string> & stringTable,         // field 6
        int64_t timeNanos,                                     // field 9
        int64_t durationNanos,                                 // field 10
        const ProtobufEncoder & periodType,                   // field 11
        int64_t period                                         // field 12
    )
    {
        for (auto & st : sampleTypes)
            writeMessageField(1, st);
        for (auto & s : samples)
            writeMessageField(2, s);
        for (auto & loc : locations)
            writeMessageField(4, loc);
        for (auto & fn : functions)
            writeMessageField(5, fn);
        for (auto & s : stringTable)
            writeBytesField(6, s);
        writeSignedVarintField(9, timeNanos);
        writeSignedVarintField(10, durationNanos);
        writeMessageField(11, periodType);
        writeSignedVarintField(12, period);
    }
};

class StringTable
{
    std::vector<std::string> strings;
    std::unordered_map<std::string, int64_t> index;

public:
    StringTable()
    {
        // Index 0 must be the empty string
        strings.emplace_back("");
        index[""] = 0;
    }

    int64_t intern(std::string_view s)
    {
        std::string key{s};
        auto it = index.find(key);
        if (it != index.end())
            return it->second;
        auto idx = static_cast<int64_t>(strings.size());
        strings.push_back(key);
        index[key] = idx;
        return idx;
    }

    const std::vector<std::string> & getStrings() const { return strings; }
};

class PprofPosCache : private LRUCache<PosIdx, Pos>
{
    const EvalState & state;

public:
    PprofPosCache(const EvalState & state)
        : LRUCache(524288)
        , state(state)
    {
    }

    Pos lookup(PosIdx posIdx)
    {
        auto posOrNone = LRUCache::get(posIdx);
        if (posOrNone)
            return *posOrNone;
        auto pos = state.positions[posIdx];
        upsert(posIdx, pos);
        return pos;
    }
};

struct FrameKey
{
    std::string name;
    std::string filename;
    int64_t line;

    bool operator==(const FrameKey &) const = default;
    auto operator<=>(const FrameKey &) const = default;
};

struct AllocSnapshot
{
    uint64_t nrValues;
    uint64_t nrEnvs;
    uint64_t nrValuesInEnvs;
    uint64_t nrAttrsets;
    uint64_t nrAttrsInAttrsets;
    uint64_t nrListElems;

    static AllocSnapshot capture(const EvalMemory::Statistics & stats)
    {
        return {
            .nrValues = stats.nrValues.load(),
            .nrEnvs = stats.nrEnvs.load(),
            .nrValuesInEnvs = stats.nrValuesInEnvs.load(),
            .nrAttrsets = stats.nrAttrsets.load(),
            .nrAttrsInAttrsets = stats.nrAttrsInAttrsets.load(),
            .nrListElems = stats.nrListElems.load(),
        };
    }

    int64_t totalBytes() const
    {
        return static_cast<int64_t>(
            nrValues * sizeof(Value)
            + nrEnvs * sizeof(Env)
            + nrValuesInEnvs * sizeof(Value *)
            + nrAttrsets * sizeof(Bindings)
            + nrAttrsInAttrsets * sizeof(Attr)
            + nrListElems * sizeof(Value *));
    }

    int64_t totalObjects() const
    {
        return static_cast<int64_t>(nrValues + nrEnvs + nrAttrsets);
    }

    AllocSnapshot operator-(const AllocSnapshot & rhs) const
    {
        return {
            .nrValues = nrValues - rhs.nrValues,
            .nrEnvs = nrEnvs - rhs.nrEnvs,
            .nrValuesInEnvs = nrValuesInEnvs - rhs.nrValuesInEnvs,
            .nrAttrsets = nrAttrsets - rhs.nrAttrsets,
            .nrAttrsInAttrsets = nrAttrsInAttrsets - rhs.nrAttrsInAttrsets,
            .nrListElems = nrListElems - rhs.nrListElems,
        };
    }
};

using StackKey = std::vector<FrameKey>;

struct SampleData
{
    int64_t cpuSamples = 0;
    int64_t allocObjects = 0;
    int64_t allocBytes = 0;
    int64_t forceCount = 0;
};

static std::atomic<bool> heapSnapshotRequested{false};

#if NIX_USE_BOEHMGC
struct HeapCensusData
{
    uint64_t valueCount = 0;
    uint64_t valueBytes = 0;
    uint64_t envCount = 0;
    uint64_t envBytes = 0;
    uint64_t bindingsCount = 0;
    uint64_t bindingsBytes = 0;
    uint64_t otherCount = 0;
    uint64_t otherBytes = 0;

    static constexpr size_t valueSize = sizeof(Value);
    static constexpr size_t envHeaderSize = sizeof(Env);
    static constexpr size_t bindingsHeaderSize = sizeof(Bindings);
};

static void GC_CALLBACK heapCensusCallback(void * obj, size_t bytes, void * clientData)
{
    auto * census = static_cast<HeapCensusData *>(clientData);

    if (bytes == HeapCensusData::valueSize) {
        census->valueCount++;
        census->valueBytes += bytes;
    } else if (bytes >= HeapCensusData::envHeaderSize
        && (bytes - HeapCensusData::envHeaderSize) % sizeof(Value *) == 0) {
        census->envCount++;
        census->envBytes += bytes;
    } else if (bytes >= HeapCensusData::bindingsHeaderSize
        && (bytes - HeapCensusData::bindingsHeaderSize) % sizeof(Attr) == 0) {
        census->bindingsCount++;
        census->bindingsBytes += bytes;
    } else {
        census->otherCount++;
        census->otherBytes += bytes;
    }
}

static void * performHeapCensusLocked(void * clientData)
{
    auto * census = static_cast<HeapCensusData *>(clientData);
    GC_enumerate_reachable_objects_inner(heapCensusCallback, census);
    return nullptr;
}
#endif

class PprofProfiler : public EvalProfiler
{
    Hooks getNeededHooksImpl() const override
    {
        return Hooks().set(preFunctionCall).set(postFunctionCall).set(preForceValue).set(postForceValue);
    }

public:
    PprofProfiler(EvalState & state, const std::filesystem::path & profileFile, uint32_t sampleInterval)
        : state(state)
        , sampleInterval(sampleInterval == 0 ? 1 : sampleInterval)
        , profilePath(profileFile)
        , posCache(state)
        , startTime(std::chrono::high_resolution_clock::now())
    {
        Counter::enabled = true;
        auto triggerPath = profilePath.parent_path() / ".nix-heap-snapshot-trigger";
        snapshotThread = std::thread([triggerPath]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::error_code ec;
                if (std::filesystem::exists(triggerPath, ec)) {
                    std::filesystem::remove(triggerPath, ec);
                    heapSnapshotRequested.store(true, std::memory_order_release);
                }
            }
        });
        snapshotThread.detach();
    }

    [[gnu::noinline]] void
    preFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos) override;
    [[gnu::noinline]] void
    postFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos) override;
    [[gnu::noinline]] void
    preForceValueHook(EvalState & state, Value & v, const PosIdx pos) override;
    [[gnu::noinline]] void
    postForceValueHook(EvalState & state, Value & v, const PosIdx pos) override;

    ~PprofProfiler() override;

private:
    FrameKey makeFrameKey(const Value & v, std::span<Value *> args, PosIdx pos);
    void writeProfile();
    void writeHeapSnapshot();
    void doSample(EvalState & state);

    EvalState & state;
    uint32_t sampleInterval;
    std::filesystem::path profilePath;
    PprofPosCache posCache;
    std::chrono::time_point<std::chrono::high_resolution_clock> startTime;

    struct PendingFrame {
        const Value * v;
        Value ** args;
        size_t nargs;
        PosIdx pos;
    };

    uint64_t callCounter = 0;
    AllocSnapshot lastAllocSnapshot = {};
    std::vector<PendingFrame> pendingStack;
    std::map<StackKey, SampleData> samples;
    std::thread snapshotThread;
};

FrameKey PprofProfiler::makeFrameKey(const Value & v, std::span<Value *> args, PosIdx pos)
{
    auto resolvedPos = posCache.lookup(pos);
    std::string filename;
    int64_t line = resolvedPos.line;

    if (auto path = std::get_if<SourcePath>(&resolvedPos.origin))
        filename = path->to_string();
    else
        filename = "<unknown>";

    std::string name;
    if (v.isLambda()) {
        auto * lambda = v.lambda().fun;
        if (lambda->name)
            name = std::string(state.symbols[lambda->name]);
        else {
            auto lambdaPos = posCache.lookup(lambda->getPos());
            std::ostringstream os;
            os << lambdaPos;
            name = "lambda@" + os.str();
        }
    } else if (v.isPrimOp()) {
        name = "primop " + std::string(v.primOp()->name);
        if (v.primOp()->name == "derivationStrict") {
            try {
                state.forceAttrs(*args[0], pos, "");
                auto attrs = args[0]->attrs();
                auto nameAttr = state.getAttr(state.s.name, attrs, "");
                auto drvName = std::string(state.forceStringNoCtx(*nameAttr->value, pos, ""));
                name = "primop derivationStrict:" + drvName;
            } catch (...) {
            }
        }
    } else if (v.isPrimOpApp()) {
        name = "primop " + std::string(v.primOpAppPrimOp()->name);
    } else if (state.isFunctor(v)) {
        name = "functor";
    } else {
        std::ostringstream os;
        os << resolvedPos;
        name = os.str();
    }

    return {.name = std::move(name), .filename = std::move(filename), .line = line};
}

[[gnu::noinline]] void
PprofProfiler::preFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos)
{
    if (heapSnapshotRequested.exchange(false, std::memory_order_acquire)) [[unlikely]]
        writeHeapSnapshot();

    pendingStack.push_back({&v, args.data(), args.size(), pos});

    if (++callCounter % sampleInterval == 0) [[unlikely]]
        doSample(state);
}

void PprofProfiler::doSample(EvalState & state)
{
    auto currentAllocs = AllocSnapshot::capture(state.mem.getStats());
    auto delta = currentAllocs - lastAllocSnapshot;
    lastAllocSnapshot = currentAllocs;

    // Resolve the pending stack to full FrameKeys
    StackKey stack;
    stack.reserve(pendingStack.size());
    for (auto & pf : pendingStack)
        stack.push_back(makeFrameKey(*pf.v, {pf.args, pf.nargs}, pf.pos));

    auto & sd = samples[stack];
    sd.cpuSamples++;
    sd.allocObjects += delta.totalObjects();
    sd.allocBytes += delta.totalBytes();
}

[[gnu::noinline]] void
PprofProfiler::postFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos)
{
    if (!pendingStack.empty())
        pendingStack.pop_back();
}

[[gnu::noinline]] void
PprofProfiler::preForceValueHook(EvalState & state, Value & v, const PosIdx pos)
{
}

[[gnu::noinline]] void
PprofProfiler::postForceValueHook(EvalState & state, Value & v, const PosIdx pos)
{
}

void PprofProfiler::writeProfile()
{
    StringTable strings;
    std::map<FrameKey, uint64_t> functionIds;
    std::map<FrameKey, uint64_t> locationIds;
    std::vector<ProtobufEncoder> functions;
    std::vector<ProtobufEncoder> locations;

    uint64_t nextFunctionId = 1;
    uint64_t nextLocationId = 1;

    auto getOrCreateFunction = [&](const FrameKey & frame) -> uint64_t {
        auto it = functionIds.find(frame);
        if (it != functionIds.end())
            return it->second;

        auto id = nextFunctionId++;
        auto nameIdx = strings.intern(frame.name);
        auto filenameIdx = strings.intern(frame.filename);
        functions.push_back(ProtobufEncoder::function(id, nameIdx, nameIdx, filenameIdx, frame.line));
        functionIds[frame] = id;
        return id;
    };

    auto getOrCreateLocation = [&](const FrameKey & frame) -> uint64_t {
        auto it = locationIds.find(frame);
        if (it != locationIds.end())
            return it->second;

        auto funcId = getOrCreateFunction(frame);
        auto id = nextLocationId++;
        std::vector<ProtobufEncoder> lines;
        lines.push_back(ProtobufEncoder::line(funcId, frame.line));
        locations.push_back(ProtobufEncoder::location(id, lines));
        locationIds[frame] = id;
        return id;
    };

    // Build sample types: [samples/count, alloc_objects/count, alloc_space/bytes, force/count]
    auto samplesIdx = strings.intern("samples");
    auto countIdx = strings.intern("count");
    auto allocObjectsIdx = strings.intern("alloc_objects");
    auto allocSpaceIdx = strings.intern("alloc_space");
    auto bytesIdx = strings.intern("bytes");
    auto forceIdx = strings.intern("force");

    std::vector<ProtobufEncoder> sampleTypes;
    sampleTypes.push_back(ProtobufEncoder::valueType(samplesIdx, countIdx));
    sampleTypes.push_back(ProtobufEncoder::valueType(allocObjectsIdx, countIdx));
    sampleTypes.push_back(ProtobufEncoder::valueType(allocSpaceIdx, bytesIdx));
    sampleTypes.push_back(ProtobufEncoder::valueType(forceIdx, countIdx));

    // Build samples
    std::vector<ProtobufEncoder> sampleMessages;
    for (auto & [stack, data] : samples) {
        if (data.cpuSamples == 0 && data.allocObjects == 0 && data.allocBytes == 0 && data.forceCount == 0)
            continue;

        // Location IDs in leaf-to-root order (pprof convention)
        std::vector<uint64_t> locIds;
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            locIds.push_back(getOrCreateLocation(*it));

        std::vector<int64_t> values = {data.cpuSamples, data.allocObjects, data.allocBytes, data.forceCount};
        sampleMessages.push_back(ProtobufEncoder::sample(locIds, values));
    }

    // Timing
    auto endTime = std::chrono::high_resolution_clock::now();
    auto wallStartNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch() - (endTime - startTime)).count();
    auto durationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();

    // Period type and value (call-count sampling, expressed as "calls" unit)
    auto callsTypeIdx = strings.intern("calls");
    auto callsUnitIdx = strings.intern("count");
    auto periodType = ProtobufEncoder::valueType(callsTypeIdx, callsUnitIdx);
    int64_t periodValue = sampleInterval;

    // Encode the profile
    ProtobufEncoder profile;
    profile.writeProfile(
        sampleTypes,
        sampleMessages,
        locations,
        functions,
        strings.getStrings(),
        wallStartNanos,
        durationNanos,
        periodType,
        periodValue
    );

    // Write to file
    auto fd = openNewFileForWrite(
        profilePath,
        0660,
        {
            .truncateExisting = true,
            .followSymlinksOnTruncate = true,
        });
    if (!fd)
        throw SysError("opening file %s", PathFmt(profilePath));

    writeFull(fd.get(), profile.data());
}

void PprofProfiler::writeHeapSnapshot()
{
#if NIX_USE_BOEHMGC
    GC_gcollect();

    HeapCensusData census;
    GC_call_with_alloc_lock(performHeapCensusLocked, &census);

    uint64_t liveBytes = census.valueBytes + census.envBytes + census.bindingsBytes + census.otherBytes;
    uint64_t liveObjects = census.valueCount + census.envCount + census.bindingsCount + census.otherCount;

    // Compute total allocated bytes across all stacks for proportional scaling
    int64_t totalAllocBytes = 0;
    for (auto & [stack, data] : samples)
        totalAllocBytes += data.allocBytes;

    StringTable strings;
    std::map<FrameKey, uint64_t> functionIds;
    std::map<FrameKey, uint64_t> locationIds;
    std::vector<ProtobufEncoder> functions;
    std::vector<ProtobufEncoder> locations;

    uint64_t nextFunctionId = 1;
    uint64_t nextLocationId = 1;

    auto getOrCreateFunction = [&](const FrameKey & frame) -> uint64_t {
        auto it = functionIds.find(frame);
        if (it != functionIds.end())
            return it->second;
        auto id = nextFunctionId++;
        auto nameIdx = strings.intern(frame.name);
        auto filenameIdx = strings.intern(frame.filename);
        functions.push_back(ProtobufEncoder::function(id, nameIdx, nameIdx, filenameIdx, frame.line));
        functionIds[frame] = id;
        return id;
    };

    auto getOrCreateLocation = [&](const FrameKey & frame) -> uint64_t {
        auto it = locationIds.find(frame);
        if (it != locationIds.end())
            return it->second;
        auto funcId = getOrCreateFunction(frame);
        auto id = nextLocationId++;
        std::vector<ProtobufEncoder> lines;
        lines.push_back(ProtobufEncoder::line(funcId, frame.line));
        locations.push_back(ProtobufEncoder::location(id, lines));
        locationIds[frame] = id;
        return id;
    };

    // Build samples: attribute live heap proportionally to allocating stacks.
    // Each stack's share of live objects = (stack's alloc bytes / total alloc bytes) * live bytes.
    std::vector<ProtobufEncoder> sampleMessages;

    if (totalAllocBytes > 0) {
        for (auto & [stack, data] : samples) {
            if (data.allocBytes <= 0)
                continue;

            double fraction = static_cast<double>(data.allocBytes) / static_cast<double>(totalAllocBytes);
            int64_t inuseObjects = static_cast<int64_t>(fraction * liveObjects);
            int64_t inuseBytes = static_cast<int64_t>(fraction * liveBytes);

            if (inuseObjects == 0 && inuseBytes == 0)
                continue;

            std::vector<uint64_t> locIds;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                locIds.push_back(getOrCreateLocation(*it));

            std::vector<int64_t> values = {inuseObjects, inuseBytes};
            sampleMessages.push_back(ProtobufEncoder::sample(locIds, values));
        }
    }

    auto inuseObjectsIdx = strings.intern("inuse_objects");
    auto inuseSpaceIdx = strings.intern("inuse_space");
    auto countIdx = strings.intern("count");
    auto bytesIdx = strings.intern("bytes");

    std::vector<ProtobufEncoder> sampleTypes;
    sampleTypes.push_back(ProtobufEncoder::valueType(inuseObjectsIdx, countIdx));
    sampleTypes.push_back(ProtobufEncoder::valueType(inuseSpaceIdx, bytesIdx));

    auto now = std::chrono::system_clock::now();
    auto timeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    auto spaceIdx = strings.intern("space");
    auto periodType = ProtobufEncoder::valueType(spaceIdx, bytesIdx);

    ProtobufEncoder profile;
    profile.writeProfile(
        sampleTypes,
        sampleMessages,
        locations,
        functions,
        strings.getStrings(),
        timeNanos,
        0,
        periodType,
        1
    );

    auto snapshotPath = profilePath.parent_path()
        / ("nix-heap-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()) + ".pb");

    auto fd = openNewFileForWrite(
        snapshotPath,
        0660,
        {
            .truncateExisting = true,
            .followSymlinksOnTruncate = true,
        });
    if (!fd)
        return;

    writeFull(fd.get(), profile.data());
#endif
}

PprofProfiler::~PprofProfiler()
{
    try {
        writeProfile();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

} // namespace

ref<EvalProfiler> makePprofProfiler(EvalState & state, std::filesystem::path profileFile, uint32_t sampleInterval)
{
    return make_ref<PprofProfiler>(state, profileFile, sampleInterval);
}

} // namespace nix
