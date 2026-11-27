// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "analysis_resources.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace seabass::gui
{

namespace
{

#if defined(__linux__)
// "0-7,16-23" as the numbers it names; empty for a file that is not there.
std::vector<int> cpuListOf(const std::string &file)
{
    std::vector<int> cpus;
    std::ifstream in(file);
    std::string list;
    if (!in || !std::getline(in, list)) {
        return cpus;
    }
    std::stringstream parts(list);
    std::string part;
    while (std::getline(parts, part, ',')) {
        try {
            const std::size_t dash = part.find('-');
            const int first = std::stoi(part.substr(0, dash));
            const int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
            for (int cpu = first; cpu <= last && cpus.size() < 4096; ++cpu) {
                cpus.push_back(cpu);
            }
        } catch (const std::exception &) {
            return {};
        }
    }
    return cpus;
}

// The cores those threads are on: two threads of one core count once.
int coresOf(const std::vector<int> &cpus)
{
    std::set<std::pair<std::string, std::string>> cores;
    for (const int cpu : cpus) {
        const std::string topology = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        std::string package;
        std::string core;
        std::ifstream packageFile(topology + "physical_package_id");
        std::ifstream coreFile(topology + "core_id");
        if (!std::getline(packageFile, package) || !std::getline(coreFile, core)) {
            // No topology for it: a core of its own.
            package = "cpu";
            core = std::to_string(cpu);
        }
        cores.insert({package, core});
    }
    return static_cast<int>(cores.size());
}
#endif

}  // namespace

CpuCores cpuCores()
{
    CpuCores cores;
#if defined(__APPLE__)
    const auto count = [](const char *name) {
        int value = 0;
        std::size_t size = sizeof(value);
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value > 0 ? value : 0;
    };
    cores.performance = count("hw.perflevel0.physicalcpu");
    cores.efficiency = count("hw.perflevel1.physicalcpu");
    if (cores.performance == 0) {
        // An Intel Mac has no performance levels.
        cores.performance = count("hw.physicalcpu");
        cores.efficiency = 0;
    }
#elif defined(__linux__)
    const std::vector<int> strong = cpuListOf("/sys/devices/cpu_core/cpus");
    const std::vector<int> weak = cpuListOf("/sys/devices/cpu_atom/cpus");
    if (!strong.empty()) {
        cores.performance = coresOf(strong);
        cores.efficiency = coresOf(weak);
    } else {
        cores.performance = coresOf(cpuListOf("/sys/devices/system/cpu/online"));
    }
#elif defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<char> buffer(length);
    if (length > 0
        && GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
        // The highest efficiency class a core has is the strong one.
        std::vector<int> classes;
        for (DWORD at = 0; at < length;) {
            const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + at);
            if (info->Size == 0) {
                break;
            }
            if (info->Relationship == RelationProcessorCore) {
                classes.push_back(info->Processor.EfficiencyClass);
            }
            at += info->Size;
        }
        if (!classes.empty()) {
            const int strongest = *std::max_element(classes.begin(), classes.end());
            cores.performance = static_cast<int>(std::count(classes.begin(), classes.end(), strongest));
            cores.efficiency = static_cast<int>(classes.size()) - cores.performance;
        }
    }
#endif
    return cores;
}

int analysisJobsFor(CpuCores cores, int threads)
{
    const int weighed =
        cores.performance > 0 || cores.efficiency > 0 ? cores.performance + cores.efficiency / 2 : threads / 2;
    return std::clamp(weighed, 2, 8);
}

void markAnalysisThread()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

std::optional<std::uint64_t> availableMemoryBytes()
{
#if defined(__APPLE__)
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    // Asked a few times a second while a track waits: the port is given
    // back each time, or its references pile up.
    const mach_port_t host = mach_host_self();
    const bool read = host_page_size(host, &page) == KERN_SUCCESS
                      && host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count)
                             == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (!read) {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(stats.free_count) + stats.inactive_count + stats.purgeable_count)
           * static_cast<std::uint64_t>(page);
#elif defined(__linux__)
    std::ifstream in("/proc/meminfo");
    std::string name;
    std::uint64_t kilobytes = 0;
    std::string unit;
    while (in >> name >> kilobytes) {
        if (name == "MemAvailable:") {
            return kilobytes * 1024;
        }
        std::getline(in, unit);
    }
    return std::nullopt;
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(status.ullAvailPhys);
#else
    return std::nullopt;
#endif
}

std::uint64_t analysisBytesFor(double seconds, int sampleRateHz)
{
    const double minutes = std::max(0.0, seconds) / 60.0;
    return AnalysisBaseBytes + static_cast<std::uint64_t>(minutes * static_cast<double>(AnalysisBytesPerMinute))
           + (sampleRateHz >= AnalysisHighRateHz ? AnalysisHighRateBytes : 0);
}

bool analysisMayStart(std::optional<std::uint64_t> availableBytes, std::uint64_t stillToTakeBytes, int inHand)
{
    if (inHand <= 0) {
        return true;
    }
    const std::uint64_t available = availableBytes.value_or(AnalysisMemoryWhenUnknown);
    const std::uint64_t needed = stillToTakeBytes + AnalysisTypicalBytes + AnalysisMemoryMargin;
    return available >= needed;
}

AnalysisMemoryGate::AnalysisMemoryGate(Reader available, Clock clock)
    : m_available(std::move(available))
    , m_clock(std::move(clock))
{
}

std::uint64_t AnalysisMemoryGate::stillToTakeLocked() const
{
    std::uint64_t bytes = 0;
    for (const auto &[ticket, track] : m_tracks) {
        bytes += track.peakBytes > track.heldBytes ? track.peakBytes - track.heldBytes : 0;
    }
    return bytes;
}

std::optional<std::uint64_t> AnalysisMemoryGate::readAvailable(std::unique_lock<std::mutex> &lock)
{
    // Reading /proc/meminfo, or asking the Mach host, while the other
    // tracks' pieces wait for the lock would hold them all up.
    lock.unlock();
    const std::optional<std::uint64_t> reading = m_available ? m_available() : std::nullopt;
    lock.lock();
    m_lastReading = reading;
    m_lastReadAt = m_clock();
    return reading;
}

int AnalysisMemoryGate::enter(const application::CancellationToken &cancel)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        cancel.throwIfCancelled();
        const std::optional<std::uint64_t> available = readAvailable(lock);
        if (analysisMayStart(available, stillToTakeLocked(), static_cast<int>(m_tracks.size()))) {
            const int ticket = m_nextTicket++;
            m_tracks[ticket] = Track{};
            return ticket;
        }
        // Woken when a track leaves; looked at again a few times a second
        // all the same, for memory something else gave back and a cancel.
        m_left.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void AnalysisMemoryGate::noteRead(int ticket, double seconds, int sampleRateHz,
                                  const application::CancellationToken &cancel)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto track = m_tracks.find(ticket);
    if (track == m_tracks.end()) {
        return;
    }
    track->second.heldBytes = analysisBytesFor(seconds, sampleRateHz);
    // The typical share at the track's own rate, now that the rate is known.
    track->second.peakBytes = std::max(analysisBytesFor(AnalysisTypicalSeconds, sampleRateHz), track->second.heldBytes);
    bool waited = false;
    for (;;) {
        cancel.throwIfCancelled();
        // Tickets count up, so the first in the map is the oldest in hand.
        if (m_tracks.empty() || m_tracks.begin()->first == ticket) {
            return;
        }
        // The gate's last reading while it is young enough; a track that
        // has waited looks again.
        std::optional<std::uint64_t> free;
        if (!waited && m_lastReading && m_clock() - m_lastReadAt < AnalysisMemoryReadingMaxAge) {
            free = *m_lastReading;
        } else {
            free = readAvailable(lock);
            // The lock was let go for the read: an older track may have
            // left meanwhile.
            if (m_tracks.empty() || m_tracks.begin()->first == ticket) {
                return;
            }
        }
        if (!free || *free >= AnalysisMemoryMargin) {
            return;
        }
        // Woken when a track leaves (this one may have become the oldest);
        // looked at again a few times a second for memory given back and
        // a cancel.
        m_left.wait_for(lock, std::chrono::milliseconds(200));
        waited = true;
    }
}

void AnalysisMemoryGate::leave(int ticket)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_tracks.erase(ticket);
    }
    m_left.notify_all();
}

int AnalysisMemoryGate::inHand() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_tracks.size());
}

std::uint64_t AnalysisMemoryGate::stillToTake() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return stillToTakeLocked();
}

AnalysisMemoryGate &AnalysisMemoryGate::shared()
{
    static AnalysisMemoryGate gate;
    return gate;
}

}  // namespace seabass::gui
