#include "nix/expr/eval-profiler.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/counter.hh"
#include "nix/util/lru-cache.hh"

#include <chrono>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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

class PprofProfiler : public EvalProfiler
{
    Hooks getNeededHooksImpl() const override
    {
        return Hooks().set(preFunctionCall).set(postFunctionCall).set(preForceValue).set(postForceValue);
    }

public:
    PprofProfiler(EvalState & state, const std::filesystem::path & profileFile, std::chrono::nanoseconds period)
        : state(state)
        , sampleInterval(period)
        , profilePath(profileFile)
        , posCache(state)
        , startTime(std::chrono::high_resolution_clock::now())
    {
        Counter::enabled = true;
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

    EvalState & state;
    std::chrono::nanoseconds sampleInterval;
    std::filesystem::path profilePath;
    PprofPosCache posCache;
    std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSample =
        std::chrono::high_resolution_clock::now();

    std::vector<FrameKey> frameStack;
    std::vector<AllocSnapshot> allocStack;
    std::map<StackKey, SampleData> samples;
    uint64_t forceDepth = 0;
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
    frameStack.push_back(makeFrameKey(v, args, pos));
    allocStack.push_back(AllocSnapshot::capture(state.mem.getStats()));

    auto now = std::chrono::high_resolution_clock::now();
    if (now - lastSample > sampleInterval) {
        samples[frameStack].cpuSamples++;
        lastSample = now;
    }
}

[[gnu::noinline]] void
PprofProfiler::postFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos)
{
    if (frameStack.empty())
        return;

    auto entryAllocs = allocStack.back();
    allocStack.pop_back();
    auto currentAllocs = AllocSnapshot::capture(state.mem.getStats());
    auto delta = currentAllocs - entryAllocs;

    auto & sd = samples[frameStack];
    sd.allocObjects += delta.totalObjects();
    sd.allocBytes += delta.totalBytes();

    frameStack.pop_back();
}

[[gnu::noinline]] void
PprofProfiler::preForceValueHook(EvalState & state, Value & v, const PosIdx pos)
{
    forceDepth++;
}

[[gnu::noinline]] void
PprofProfiler::postForceValueHook(EvalState & state, Value & v, const PosIdx pos)
{
    if (forceDepth > 0)
        forceDepth--;

    if (!frameStack.empty()) {
        samples[frameStack].forceCount++;
    }
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

    // Period type and value
    auto cpuIdx = strings.intern("cpu");
    auto nanosecondsIdx = strings.intern("nanoseconds");
    auto periodType = ProtobufEncoder::valueType(cpuIdx, nanosecondsIdx);
    auto periodNanos = sampleInterval.count() > 0 ? sampleInterval.count() : 1;

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
        periodNanos
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

PprofProfiler::~PprofProfiler()
{
    try {
        writeProfile();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

} // namespace

ref<EvalProfiler> makePprofProfiler(EvalState & state, std::filesystem::path profileFile, uint64_t frequency)
{
    std::chrono::nanoseconds period = frequency == 0
                                          ? std::chrono::nanoseconds{0}
                                          : std::chrono::nanoseconds{std::nano::den / frequency / std::nano::num};
    return make_ref<PprofProfiler>(state, profileFile, period);
}

} // namespace nix
