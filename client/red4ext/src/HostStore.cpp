// cyber.rest — Phase 4 host persistence store implementation.
//
// In-memory map of GUID -> PlayerRecord, persisted to a small JSON file by a dedicated flush
// thread. See HostStore.hpp for the threading contract. The JSON is hand-rolled (no dependency):
// a flat object whose keys are player GUIDs and whose values are one-line records. We write it and
// parse it ourselves; parsing is deliberately forgiving (a bad field -> the record's default) so a
// truncated or hand-edited file can never crash the host.

#include "HostStore.hpp"
#include "PluginContext.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cr
{
// ---- static storage ----
std::mutex HostStore::s_mutex;
std::unordered_map<std::string, PlayerRecord> HostStore::s_records;
bool HostStore::s_dirty = false;
std::string HostStore::s_filePath;
std::thread HostStore::s_thread;
std::atomic<bool> HostStore::s_running{false};
std::condition_variable HostStore::s_cv;
std::mutex HostStore::s_cvMutex;

namespace
{
// ---- minimal JSON helpers (host-only, our own format) --------------------------------------
//
// We only ever emit/parse the exact object-of-records shape SaveToDiskLocked writes, so the parser
// is a small tolerant scanner rather than a general JSON library. Strings are our own GUIDs +
// numbers, so no escaping beyond quotes/backslash is needed; we still escape defensively on write.

std::string JsonEscape(const std::string& aIn)
{
    std::string out;
    out.reserve(aIn.size() + 2);
    for (char c : aIn)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (c == '\n' || c == '\r' || c == '\t')
        {
            // Our GUIDs never contain these, but drop them rather than emit invalid JSON.
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

// Append a "key":<number>, pair. Kept as helpers so the record writer stays readable.
void AppendU32(std::ostringstream& aOut, const char* aKey, uint32_t aVal)
{
    aOut << '"' << aKey << "\":" << aVal;
}
void AppendBool(std::ostringstream& aOut, const char* aKey, bool aVal)
{
    aOut << '"' << aKey << "\":" << (aVal ? "true" : "false");
}
void AppendFloat(std::ostringstream& aOut, const char* aKey, float aVal)
{
    // %.6g is round-trippable enough for world coords at our scale and keeps the file compact.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(aVal));
    aOut << '"' << aKey << "\":" << buf;
}
void AppendU64(std::ostringstream& aOut, const char* aKey, uint64_t aVal)
{
    aOut << '"' << aKey << "\":" << aVal;
}

// Find the value token following the first occurrence of "<key>": in aObj, starting at aFrom.
// Returns the substring from just after the colon up to the next top-level , or } (not recursing
// into nested braces/brackets, which our records don't have except the clothing array we scan
// specially). Empty string if the key isn't present. Tolerant: whitespace-trimmed.
std::string FindValue(const std::string& aObj, const std::string& aKey)
{
    const std::string needle = "\"" + aKey + "\":";
    const size_t k = aObj.find(needle);
    if (k == std::string::npos)
    {
        return std::string();
    }
    size_t i = k + needle.size();
    // Skip leading whitespace.
    while (i < aObj.size() && (aObj[i] == ' ' || aObj[i] == '\t'))
    {
        ++i;
    }
    // Collect until , or } or ] at this level (no nesting in scalar values).
    size_t start = i;
    while (i < aObj.size() && aObj[i] != ',' && aObj[i] != '}' && aObj[i] != ']')
    {
        ++i;
    }
    std::string v = aObj.substr(start, i - start);
    // Trim trailing whitespace.
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
    {
        v.pop_back();
    }
    return v;
}

float ParseFloat(const std::string& aObj, const std::string& aKey, float aDefault)
{
    const std::string v = FindValue(aObj, aKey);
    if (v.empty())
    {
        return aDefault;
    }
    try
    {
        return std::stof(v);
    }
    catch (...)
    {
        return aDefault;
    }
}
uint32_t ParseU32(const std::string& aObj, const std::string& aKey, uint32_t aDefault)
{
    const std::string v = FindValue(aObj, aKey);
    if (v.empty())
    {
        return aDefault;
    }
    try
    {
        return static_cast<uint32_t>(std::stoul(v));
    }
    catch (...)
    {
        return aDefault;
    }
}
uint64_t ParseU64(const std::string& aStr, uint64_t aDefault)
{
    std::string v = aStr;
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
    {
        v.erase(v.begin());
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
    {
        v.pop_back();
    }
    if (v.empty())
    {
        return aDefault;
    }
    try
    {
        return std::stoull(v);
    }
    catch (...)
    {
        return aDefault;
    }
}
bool ParseBool(const std::string& aObj, const std::string& aKey, bool aDefault)
{
    const std::string v = FindValue(aObj, aKey);
    if (v.empty())
    {
        return aDefault;
    }
    return v.find("true") != std::string::npos;
}
} // namespace

// ---- lifecycle ------------------------------------------------------------------------------

void HostStore::Start(const std::string& aFilePath)
{
    if (s_running.load())
    {
        return; // already started
    }
    if (s_thread.joinable())
    {
        s_thread.join(); // reap a finished-but-unjoined flush thread
    }

    s_filePath = aFilePath;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_records.clear();
        s_dirty = false;
    }
    LoadFromDisk();

    s_running.store(true);
    s_thread = std::thread(&HostStore::FlushThreadMain);
    Log::Info("HostStore started (file: " + s_filePath + ")");
}

void HostStore::Stop()
{
    if (!s_running.load() && !s_thread.joinable())
    {
        return;
    }
    // Persist the latest state, then signal the flush thread to exit and join it.
    FlushNow();
    s_running.store(false);
    s_cv.notify_all();
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    Log::Info("HostStore stopped");
}

void HostStore::FlushThreadMain()
{
    while (s_running.load())
    {
        {
            std::unique_lock<std::mutex> lock(s_cvMutex);
            // Wake on: stop requested, or the flush interval elapsed. We re-check the dirty flag
            // under s_mutex below regardless, so a spurious wake just no-ops.
            s_cv.wait_for(lock, std::chrono::seconds(kFlushIntervalSeconds),
                          [] { return !s_running.load(); });
        }

        bool needSave = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            needSave = s_dirty;
        }
        if (needSave)
        {
            SaveToDiskLocked();
        }
    }
}

// ---- mutation (worker thread) ---------------------------------------------------------------

PlayerRecord& HostStore::TouchLocked(const std::string& aGuid)
{
    auto it = s_records.find(aGuid);
    if (it == s_records.end())
    {
        PlayerRecord rec;
        rec.guid = aGuid;
        it = s_records.emplace(aGuid, rec).first;
    }
    return it->second;
}

bool HostStore::Get(const std::string& aGuid, PlayerRecord& aOut)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto it = s_records.find(aGuid);
    if (it == s_records.end())
    {
        return false;
    }
    aOut = it->second;
    return true;
}

bool HostStore::EnsureRecord(const std::string& aGuid)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const bool isNew = s_records.find(aGuid) == s_records.end();
    TouchLocked(aGuid);
    if (isNew)
    {
        s_dirty = true;
    }
    return isNew;
}

void HostStore::UpsertPosition(const std::string& aGuid, float aPx, float aPy, float aPz, float aYaw)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    PlayerRecord& r = TouchLocked(aGuid);
    r.hasPosition = true;
    r.px = aPx;
    r.py = aPy;
    r.pz = aPz;
    r.yaw = aYaw;
    s_dirty = true;
}

void HostStore::UpsertAppearance(const std::string& aGuid, uint8_t aBodyGender, const uint64_t* aClothing)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    PlayerRecord& r = TouchLocked(aGuid);
    r.hasAppearance = true;
    r.bodyGender = aBodyGender;
    for (uint32_t i = 0; i < kAppearanceSlots; ++i)
    {
        r.clothing[i] = (aClothing != nullptr) ? aClothing[i] : 0;
    }
    s_dirty = true;
}

void HostStore::RecordKill(const std::string& aGuid)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    PlayerRecord& r = TouchLocked(aGuid);
    r.kills += 1;
    s_dirty = true;
}

void HostStore::RecordDeath(const std::string& aGuid)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    PlayerRecord& r = TouchLocked(aGuid);
    r.deaths += 1;
    s_dirty = true;
}

void HostStore::GetStats(const std::string& aGuid, uint32_t& aKills, uint32_t& aDeaths)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto it = s_records.find(aGuid);
    if (it == s_records.end())
    {
        aKills = 0;
        aDeaths = 0;
        return;
    }
    aKills = it->second.kills;
    aDeaths = it->second.deaths;
}

void HostStore::FlushNow()
{
    bool needSave = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        needSave = s_dirty;
    }
    if (needSave)
    {
        SaveToDiskLocked();
    }
}

// ---- disk IO --------------------------------------------------------------------------------

void HostStore::SaveToDiskLocked()
{
    // Snapshot the records under the lock, then write outside it so disk latency never blocks a
    // worker-thread mutation for long. Clear the dirty flag as part of the snapshot so changes made
    // DURING the write mark the store dirty again (and get picked up next flush) rather than being
    // lost.
    std::unordered_map<std::string, PlayerRecord> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        snapshot = s_records;
        s_dirty = false;
    }

    std::ostringstream out;
    out << "{\n";
    bool first = true;
    for (const auto& [guid, r] : snapshot)
    {
        if (!first)
        {
            out << ",\n";
        }
        first = false;

        out << "  \"" << JsonEscape(guid) << "\":{";
        AppendBool(out, "hasPosition", r.hasPosition);
        out << ',';
        AppendFloat(out, "px", r.px);
        out << ',';
        AppendFloat(out, "py", r.py);
        out << ',';
        AppendFloat(out, "pz", r.pz);
        out << ',';
        AppendFloat(out, "yaw", r.yaw);
        out << ',';
        AppendBool(out, "hasAppearance", r.hasAppearance);
        out << ',';
        AppendU32(out, "bodyGender", r.bodyGender);
        out << ',';
        out << "\"clothing\":[";
        for (uint32_t i = 0; i < kAppearanceSlots; ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            out << r.clothing[i];
        }
        out << ']';
        out << ',';
        AppendU32(out, "kills", r.kills);
        out << ',';
        AppendU32(out, "deaths", r.deaths);
        out << '}';
    }
    out << "\n}\n";

    // Write to a temp file then rename, so an interrupted write can't corrupt the live file.
    const std::string tmpPath = s_filePath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            Log::Error("HostStore: failed to open " + tmpPath + " for write");
            return;
        }
        const std::string s = out.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    // std::remove the destination first (rename over an existing file fails on Windows), then rename.
    std::remove(s_filePath.c_str());
    if (std::rename(tmpPath.c_str(), s_filePath.c_str()) != 0)
    {
        Log::Error("HostStore: failed to rename temp file into place");
    }
}

void HostStore::LoadFromDisk()
{
    std::ifstream f(s_filePath, std::ios::binary);
    if (!f.is_open())
    {
        Log::Info("HostStore: no existing save at " + s_filePath + " (starting fresh)");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();

    // Scan top-level "guid":{ ... } entries. We don't build a full JSON tree — we walk to each
    // opening brace of a record object, capture the record body (balanced braces), and parse its
    // scalar fields with the tolerant helpers above. A malformed entry is skipped, not fatal.
    std::unordered_map<std::string, PlayerRecord> loaded;

    size_t i = 0;
    const size_t n = content.size();
    // Skip to the outer '{'.
    while (i < n && content[i] != '{')
    {
        ++i;
    }
    if (i < n)
    {
        ++i; // past the outer '{'
    }

    while (i < n)
    {
        // Find the next key string (a '"').
        while (i < n && content[i] != '"' && content[i] != '}')
        {
            ++i;
        }
        if (i >= n || content[i] == '}')
        {
            break; // end of the object
        }
        // Read the quoted key (our GUIDs contain no escaped quotes, but honor a backslash-escape).
        ++i; // past opening quote
        std::string key;
        while (i < n && content[i] != '"')
        {
            if (content[i] == '\\' && i + 1 < n)
            {
                ++i; // skip the escape, take the next char literally
            }
            key.push_back(content[i]);
            ++i;
        }
        if (i < n)
        {
            ++i; // past closing quote
        }
        // Expect ':' then '{'.
        while (i < n && content[i] != '{' && content[i] != '}')
        {
            ++i;
        }
        if (i >= n || content[i] == '}')
        {
            break;
        }
        // Capture the balanced record object body.
        const size_t objStart = i;
        int depth = 0;
        for (; i < n; ++i)
        {
            if (content[i] == '{')
            {
                ++depth;
            }
            else if (content[i] == '}')
            {
                --depth;
                if (depth == 0)
                {
                    ++i; // include the closing brace
                    break;
                }
            }
        }
        const std::string obj = content.substr(objStart, i - objStart);

        // Parse the record.
        PlayerRecord rec;
        rec.guid = key;
        rec.hasPosition = ParseBool(obj, "hasPosition", false);
        rec.px = ParseFloat(obj, "px", 0.0f);
        rec.py = ParseFloat(obj, "py", 0.0f);
        rec.pz = ParseFloat(obj, "pz", 0.0f);
        rec.yaw = ParseFloat(obj, "yaw", 0.0f);
        rec.hasAppearance = ParseBool(obj, "hasAppearance", false);
        rec.bodyGender = static_cast<uint8_t>(ParseU32(obj, "bodyGender", 0));
        rec.kills = ParseU32(obj, "kills", 0);
        rec.deaths = ParseU32(obj, "deaths", 0);

        // Parse the clothing array: find "clothing":[ ... ] and split its comma-separated u64s.
        {
            const std::string arrNeedle = "\"clothing\":[";
            const size_t a = obj.find(arrNeedle);
            if (a != std::string::npos)
            {
                const size_t arrStart = a + arrNeedle.size();
                const size_t arrEnd = obj.find(']', arrStart);
                if (arrEnd != std::string::npos)
                {
                    const std::string arr = obj.substr(arrStart, arrEnd - arrStart);
                    std::stringstream as(arr);
                    std::string tok;
                    uint32_t slot = 0;
                    while (std::getline(as, tok, ',') && slot < kAppearanceSlots)
                    {
                        rec.clothing[slot] = ParseU64(tok, 0);
                        ++slot;
                    }
                }
            }
        }

        if (!rec.guid.empty())
        {
            loaded.emplace(rec.guid, rec);
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_records = std::move(loaded);
        s_dirty = false;
    }
    Log::Info("HostStore: loaded " + std::to_string(s_records.size()) + " player record(s) from " + s_filePath);
}
} // namespace cr
