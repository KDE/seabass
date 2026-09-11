#include "infrastructure/local/stick_performance_history.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "infrastructure/local/app_data_directory.hpp"

namespace seabass::infrastructure::local
{

namespace fs = std::filesystem;

namespace
{

// Tabs and newlines cannot appear in a field: a stick label with either
// would corrupt the line, so they are folded to spaces on the way in.
std::string field(std::string value)
{
    for (char &c : value) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return value;
}

}  // namespace

fs::path StickPerformanceHistory::defaultPath()
{
    return appDataDirectory() / "stick-performance-history.tsv";
}

StickPerformanceHistory::StickPerformanceHistory(fs::path path) : m_path(std::move(path)) {}

std::vector<StickPerformanceRecord> StickPerformanceHistory::readAll() const
{
    std::vector<StickPerformanceRecord> records;
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::string current;
        std::istringstream lineStream(line);
        while (std::getline(lineStream, current, '\t')) {
            fields.push_back(current);
        }
        if (fields.size() < 8) {
            continue;  // a line this code never wrote; leave it be
        }
        try {
            StickPerformanceRecord r;
            r.measuredAtUtc = fields[0];
            r.stickIdentifier = fields[1];
            r.stickLabel = fields[2];
            r.score = std::stoi(fields[3]);
            r.streamingBytesPerSecond = std::stod(fields[4]);
            r.randomReadMedianMs = std::stod(fields[5]);
            r.smallFileMedianMs = std::stod(fields[6]);
            r.outliers = std::stoi(fields[7]);
            r.wearState = fields.size() > 8 ? fields[8] : "";
            records.push_back(r);
        } catch (const std::exception &) {
            // A malformed number: skip the line rather than lose the file.
        }
    }
    return records;
}

void StickPerformanceHistory::writeAll(const std::vector<StickPerformanceRecord> &records) const
{
    std::error_code ec;
    fs::create_directories(m_path.parent_path(), ec);
    std::ofstream out(m_path, std::ios::trunc);
    out << "# Seabass stick performance history: measured-at, stick id, label, score, streaming B/s, "
           "random read ms, small file ms, tail outliers, wear state\n";
    for (const auto &r : records) {
        out << field(r.measuredAtUtc) << '\t' << field(r.stickIdentifier) << '\t' << field(r.stickLabel) << '\t'
            << r.score << '\t' << r.streamingBytesPerSecond << '\t' << r.randomReadMedianMs << '\t'
            << r.smallFileMedianMs << '\t' << r.outliers << '\t' << field(r.wearState) << '\n';
    }
}

std::vector<StickPerformanceRecord> StickPerformanceHistory::forStick(const std::string &stickIdentifier) const
{
    std::vector<StickPerformanceRecord> out;
    for (const auto &r : readAll()) {
        if (r.stickIdentifier == stickIdentifier) {
            out.push_back(r);
        }
    }
    return out;
}

void StickPerformanceHistory::append(const StickPerformanceRecord &record)
{
    auto records = readAll();
    records.push_back(record);
    // Drop the oldest of this stick beyond the cap; other sticks untouched.
    int forThis = 0;
    for (const auto &r : records) {
        if (r.stickIdentifier == record.stickIdentifier) {
            ++forThis;
        }
    }
    for (auto it = records.begin(); it != records.end() && forThis > kKeepPerStick;) {
        if (it->stickIdentifier == record.stickIdentifier) {
            it = records.erase(it);
            --forThis;
        } else {
            ++it;
        }
    }
    writeAll(records);
}

void StickPerformanceHistory::setLatestWearState(const std::string &stickIdentifier, const std::string &wearState)
{
    auto records = readAll();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->stickIdentifier == stickIdentifier) {
            it->wearState = wearState;
            writeAll(records);
            return;
        }
    }
}

}  // namespace seabass::infrastructure::local
