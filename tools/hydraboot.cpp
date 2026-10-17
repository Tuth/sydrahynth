// Boots the Hydrasynth firmware on the emulated main board and reports how far
// it got: stop reason, CPU state, CP15 activity and every peripheral access.
//
//   hydraboot <firmware.dat> [cpu ticks, 400 MHz] [--note] [--capture file] [--wav file]
//             [--bank file.hydra [--bank-first i] [--bank-limit n] [--bank-slot s] [--bank-to b]] [--program n [--program-bank b]]
//             [--midi "hex bytes"] [--setting page.entry=value] [--flash nor.bin]
//
// --bank uploads n (default 1) patches of a bank, starting with patch i
// (default 0), to bank A over MIDI SysEx after booting, one line per patch;
// --bank-slot s stores them all in slot s instead of their own slots (an upload
// to the current program, A001 after boot, also makes it the current sound);
// --bank-to b sends them to bank b (0 = A ... 4 = E) instead of A. --program sends a
// bank select (CC 32 = b, the LSB; the firmware ignores CC 0) and program change n afterwards.
//
// --setting sets a system setting of the blank flash (e.g. 5.7=1: PgmChgRX on).
// --flash restores the user area (settings, banks) from a HYDRA_NOR_DUMP file.
// --wait runs that many more seconds before the note.
// --panel "b45 d1 e0:2 w500" operates the front panel after that (see below).
// --midi sends the given bytes (e.g. "e0 00 40") after that, before the note.
//
// --note boots for the given time, sends a MIDI note-on (channel 1, middle C)
// and runs for another second, reporting how the firmware reacted. --capture
// records, for each sample of the quiet and the note seconds, the 48 tone
// generator buses, DSP port words 64-71 and the stereo audio output after the
// effects (58 x i32 per sample); --wav writes that stereo output as a 24-bit WAV.
//
// Environment: HYDRA_DSP_DUMP / HYDRA_FX_DUMP / HYDRA_NOR_DUMP / HYDRA_DRAM_DUMP write
// the DSP block's / effects DSPs' program memory, NOR flash or DRAM to a file; HYDRA_PANEL_LOG lists panel MCU peripheral accesses;
// HYDRA_MAIN_LOG also logs main-bus accesses to modelled devices during the
// note; HYDRA_VOICE_DEBUG follows each voice's amp envelope during the note;
// HYDRA_SCREEN=file.pgm writes the five panel displays at the end of the run;
// HYDRA_HOST_PROFILE breaks the main thread's time down after boot;
// HYDRA_BLOCK_PROFILE lists the main CPU's hottest translated blocks (exact),
// HYDRA_PANEL_BLOCK_PROFILE the panel MCU's;
// HYDRA_GUEST_PROFILE lists the main CPU's hottest PCs after boot;
// HYDRA_PROFILE prints wall time per component; HYDRA_SAMPLE=file samples the main
// thread (see below); HYDRA_PC_SAMPLES lists the hottest
// slice-end PCs; HYDRA_FX_THREAD runs the effects on
// their own thread; HYDRA_DSP_ASYNC runs the whole DSP side concurrently with the CPU; HYDRA_NO_DELAY_SKIP runs delay loops
// instead of fast-forwarding them; HYDRA_DSP_THREADS=n runs the DSP
// block's cores on n threads;
// HYDRA_WATCH=from-to (hex) lists the code writing that range (from boot, shown after
// the note), HYDRA_WATCH_VALUE=v (hex) only writes of that value.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include <dynarmic/frontend/A32/disassembler/disassembler.h>

#include "a5pack.h"
#include "machine.h"
#include "patch_bank.h"
#include "sysex.h"

using namespace hydra;

// HYDRA_SAMPLE=file: samples the main thread's instruction pointer at 5 kHz of its
// CPU time (with HYDRA_SAMPLE_ALL: of all threads, by process CPU time) and writes /proc/self/maps, "SAMPLES" and one hex address per line;
// tools/symbolize_samples.py turns that into a per-function profile.
#include <csignal>
#include <ctime>
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace {
#if defined(__linux__)
constexpr std::size_t kMaxSamples = 1 << 20;
std::uintptr_t gSamples[kMaxSamples];
std::atomic<std::size_t> gSampleCount{0};

void recordSample(int, siginfo_t*, void* context) {
    const std::size_t i = gSampleCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxSamples)
        gSamples[i] = static_cast<std::uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}

void startSampler() {
    struct sigaction action {};
    action.sa_sigaction = recordSample;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    if (std::getenv("HYDRA_SAMPLE_ALL")) {
        // All threads: SIGPROF goes to whichever thread is using the CPU.
        sigaction(SIGPROF, &action, nullptr);
        itimerval interval{};
        interval.it_interval.tv_usec = 200;
        interval.it_value.tv_usec = 200;
        setitimer(ITIMER_PROF, &interval, nullptr);
        return;
    }
    sigaction(SIGRTMIN + 3, &action, nullptr);
    sigevent event{};
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGRTMIN + 3;
    event._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
    timer_t timer;
    timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer);
    itimerspec interval{};
    interval.it_interval.tv_nsec = 200000;
    interval.it_value.tv_nsec = 200000;
    timer_settime(timer, 0, &interval, nullptr);
}

void writeSamples(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    if (std::FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, maps))
            std::fputs(line, f);
        std::fclose(maps);
    }
    std::fprintf(f, "SAMPLES\n");
    for (std::size_t i = 0; i < std::min(gSampleCount.load(), kMaxSamples); ++i)
        std::fprintf(f, "%lx\n", static_cast<unsigned long>(gSamples[i]));
    std::fclose(f);
}
#else
void startSampler() { std::fprintf(stderr, "HYDRA_SAMPLE is available on Linux only; use a native Windows CPU profiler.\n"); }
void writeSamples(const char*) {}
#endif
}  // namespace

// Writes interleaved stereo samples (24-bit range, clamped) as a 44.1 kHz 24-bit WAV.
bool writeWav(const char* path, const std::vector<std::int32_t>& samples) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    const auto u32 = [f](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [f](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 3);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);  // PCM
    u16(2);
    u32(44100);
    u32(44100 * 2 * 3);
    u16(2 * 3);
    u16(24);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (const std::int32_t sample : samples) {
        const std::int32_t v = std::clamp(sample, -0x800000, 0x7FFFFF);
        const std::uint8_t b[3] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16)};
        std::fwrite(b, 1, 3, f);
    }
    return std::fclose(f) == 0;
}

namespace {
// All five panel displays side by side, main display first, as an 8-bit PGM.
void writeScreen(PanelMcu& panel, const std::string& path) {
    constexpr int kW = Oled::kWidth, kH = Oled::kHeight, kN = PanelDisplays::kCount;
    std::vector<std::uint8_t> image(static_cast<std::size_t>(kW * kN * kH));
    Oled::Image screen;
    for (int d = 0; d < kN; ++d) {
        panel.displays().display(d).render(screen);
        for (int y = 0; y < kH; ++y)
            std::copy_n(&screen[static_cast<std::size_t>(y * kW)], kW, &image[static_cast<std::size_t>((y * kN + d) * kW)]);
    }
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P5\n%d %d\n255\n", kW * kN, kH);
        std::fwrite(image.data(), 1, image.size(), f);
        std::fclose(f);
    }
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    bool note = false;
    const char* capturePath = nullptr;
    const char* wavPath = nullptr;
    const char* bankPath = nullptr;
    std::size_t bankLimit = 1, bankFirst = 0;
    int program = -1, bankSlot = -1, bankTo = 0, programBank = 0;
    std::vector<std::uint8_t> extraMidi;
    double waitSeconds = 0;
    std::string panelScript;
    Machine::Config config;
    config.effectsThread = std::getenv("HYDRA_FX_THREAD") != nullptr;
    config.dspThread = std::getenv("HYDRA_DSP_ASYNC") != nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--note") == 0)
            note = true;
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc)
            capturePath = argv[++i];
        else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
            wavPath = argv[++i];
        else if (std::strcmp(argv[i], "--bank") == 0 && i + 1 < argc)
            bankPath = argv[++i];
        else if (std::strcmp(argv[i], "--program") == 0 && i + 1 < argc)
            program = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--program-bank") == 0 && i + 1 < argc)
            programBank = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bank-to") == 0 && i + 1 < argc)
            bankTo = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--panel") == 0 && i + 1 < argc)
            panelScript = argv[++i];
        else if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
            waitSeconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            // A NOR dump (HYDRA_NOR_DUMP): restore the non-blank sectors of its user area.
            std::FILE* f = std::fopen(argv[++i], "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", argv[i]);
                return 2;
            }
            std::vector<std::uint8_t> sector(0x20000);
            for (std::uint32_t at = 0x7000000; at < 0x8000000; at += 0x20000) {
                if (std::fseek(f, at, SEEK_SET) != 0 || std::fread(sector.data(), 1, sector.size(), f) != sector.size())
                    break;
                if (std::any_of(sector.begin(), sector.end(), [](std::uint8_t b) { return b != 0xFF; }))
                    config.flash.push_back({at, sector});
            }
            std::fclose(f);
            std::printf("restoring %zu flash sectors\n", config.flash.size());
        } else if (std::strcmp(argv[i], "--setting") == 0 && i + 1 < argc) {
            // page.entry=value, e.g. 5.7=1 (PgmChgRX on)
            int page = 0, entry = 0, value = 0;
            if (std::sscanf(argv[++i], "%d.%d=%d", &page, &entry, &value) != 3) {
                std::fprintf(stderr, "--setting expects page.entry=value\n");
                return 2;
            }
            config.systemSettings[{page, entry}] = static_cast<std::int16_t>(value);
        } else if (std::strcmp(argv[i], "--midi") == 0 && i + 1 < argc) {
            // Hex bytes, e.g. "e0 00 40" or "e00040".
            for (const char* p = argv[++i]; *p;) {
                if (*p == ' ' || *p == ',') {
                    ++p;
                    continue;
                }
                char hex[3] = {p[0], p[1] ? p[1] : '\0', '\0'};
                extraMidi.push_back(static_cast<std::uint8_t>(std::strtoul(hex, nullptr, 16)));
                p += p[1] ? 2 : 1;
            }
        } else if (std::strcmp(argv[i], "--bank-slot") == 0 && i + 1 < argc)
            bankSlot = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bank-first") == 0 && i + 1 < argc)
            bankFirst = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--bank-limit") == 0 && i + 1 < argc)
            bankLimit = std::strtoul(argv[++i], nullptr, 0);
        else
            positional.emplace_back(argv[i]);
    }
    if (positional.empty()) {
        std::fprintf(stderr, "usage: %s <firmware.dat> [cpu ticks] [--note] [--capture file] [--wav file] [--bank file.hydra [--bank-first i] [--bank-limit n] [--bank-slot s] [--bank-to b]] [--program n [--program-bank b]] [--midi \"hex bytes\"] [--setting page.entry=value] [--flash nor.bin] [--wait seconds]\n", argv[0]);
        return 2;
    }
    const std::uint64_t budget = positional.size() > 1 ? std::strtoull(positional[1].c_str(), nullptr, 0) : 400'000'000;

    try {
        const FirmwarePack firmware = FirmwarePack::load(positional[0]);
        std::printf("firmware: model %s\n", firmware.model.c_str());
        for (const auto& section : firmware.sections)
            std::printf("  %-14s v%-4s %8zu bytes\n", section.name.c_str(), section.version.c_str(), section.data.size());

        Machine machine(firmware, config);
        machine.dspBlock().diagnostics = true;
        const bool panelLog = std::getenv("HYDRA_PANEL_LOG") != nullptr;
        machine.panel().bus().log().all = panelLog;

        std::array<std::vector<std::uint8_t>, 3> transmitted;
        for (int i = 0; i < 3; ++i)
            machine.uart(i).monitor = [&transmitted, i](std::uint8_t byte) { transmitted[i].push_back(byte); };

        // HYDRA_WATCH=from-to (hex): count the code blocks writing that range, from boot on.
        const char* watch = std::getenv("HYDRA_WATCH");
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint64_t, std::uint64_t>> watchWriters;  // (block pc, lr) -> writes, last value
        if (watch) {
            char* end = nullptr;
            const auto from = static_cast<std::uint32_t>(std::strtoul(watch, &end, 16));
            machine.cpu().setWatch(from, static_cast<std::uint32_t>(std::strtoul(end + (*end == '-'), nullptr, 16)));
            // HYDRA_WATCH_VALUE (hex): only count writes of that value.
            const char* watchValue = std::getenv("HYDRA_WATCH_VALUE");
            const std::uint64_t wanted = watchValue ? std::strtoull(watchValue, nullptr, 16) : 0;
            machine.cpu().onWatchedWrite = [&, watchValue, wanted](std::uint32_t pc, std::uint32_t lr, std::uint32_t, std::uint64_t value) {
                if (watchValue && value != wanted)
                    return;
                auto& e = watchWriters[{pc, lr}];
                ++e.first;
                e.second = value;
            };
        }

        machine.profile().enabled = std::getenv("HYDRA_PROFILE") != nullptr;
        static std::map<std::uint64_t, std::uint64_t> delayCallers;
        if (std::getenv("HYDRA_DELAY_CALLERS"))
            machine.cpu().delayCallers = &delayCallers;
        machine.recordPcSamples = std::getenv("HYDRA_PC_SAMPLES") != nullptr;
        machine.cpu().delayLoopSkip = std::getenv("HYDRA_NO_DELAY_SKIP") == nullptr;
        if (const char* threads = std::getenv("HYDRA_DSP_THREADS"))
            machine.dspBlock().setThreads(std::atoi(threads));
        const char* samplePath = std::getenv("HYDRA_SAMPLE");
        if (samplePath)
            startSampler();
        Machine::Status status = machine.run(budget);
        if (std::getenv("HYDRA_DELAY_CALLERS")) {
            std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> byCaller;  // lr -> entries, iterations
            for (const auto& [key, count] : delayCallers) {
                auto& e = byCaller[static_cast<std::uint32_t>(key >> 32)];
                e.first += count;
                e.second += count * (key & 0xFFFFFFFF);
            }
            std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
            for (const auto& [lr, e] : byCaller)
                order.emplace_back(e.first, lr);
            std::sort(order.rbegin(), order.rend());
            std::map<std::uint32_t, std::uint64_t> byCount;
            for (const auto& [key, count] : delayCallers)
                byCount[static_cast<std::uint32_t>(key & 0xFFFFFFFF)] += count;
            std::printf("delay loop entries by count:");
            int shown = 0;
            for (const auto& [n, count] : byCount)
                if (count > 1000 && shown++ < 20)
                    std::printf(" %u:%llu", n, static_cast<unsigned long long>(count));
            std::printf("\n");
            {
                std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> longByCaller;  // lr -> entries, iterations (n > 256)
                for (const auto& [key, count] : delayCallers)
                    if ((key & 0xFFFFFFFF) > 256) {
                        auto& e = longByCaller[static_cast<std::uint32_t>(key >> 32)];
                        e.first += count;
                        e.second += count * (key & 0xFFFFFFFF);
                    }
                std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
                for (const auto& [lr, e] : longByCaller)
                    order.emplace_back(e.second, lr);
                std::sort(order.rbegin(), order.rend());
                std::printf("long delays (> 256) by caller (lr: entries, emulated seconds):\n");
                for (std::size_t i = 0; i < std::min<std::size_t>(order.size(), 12); ++i)
                    std::printf("  %08x: %llu, %.3f s\n", order[i].second, static_cast<unsigned long long>(longByCaller[order[i].second].first),
                                3.0 * static_cast<double>(order[i].first) / Machine::kCpuHz);
            }
            std::printf("delay loop entries by caller (lr: entries, mean count):\n");
            for (std::size_t i = 0; i < std::min<std::size_t>(order.size(), 15); ++i)
                std::printf("  %08x: %llu, %.0f\n", order[i].second, static_cast<unsigned long long>(order[i].first),
                            static_cast<double>(byCaller[order[i].second].second) / static_cast<double>(order[i].first));
        }
        if (samplePath)
            writeSamples(samplePath);

        if (bankPath && status != Machine::Status::Fatal) {
            std::vector<Patch> patches = loadPatches(bankPath);
            patches.erase(patches.begin(), patches.begin() + static_cast<std::ptrdiff_t>(std::min(bankFirst, patches.size())));
            if (patches.size() > bankLimit)
                patches.resize(bankLimit);
            for (const Patch& patch : patches) {
                int acknowledged = 0, missing = 0;
                std::string replies;
                for (const auto& message : sysex::patchUpload(patch.body, bankTo, bankSlot >= 0 ? bankSlot : patch.slot, true)) {
                    const std::size_t before = transmitted[0].size();
                    machine.sendMidi(message);
                    // 31250 baud: 320 us per byte; then up to 50 ms for the acknowledgement.
                    status = machine.run(static_cast<std::uint64_t>(message.size()) * Machine::kCpuHz * 320 / 1000000);
                    std::size_t end = 0;
                    for (int wait = 0; wait <= 50 && status != Machine::Status::Fatal; ++wait) {
                        for (std::size_t i = before; i < transmitted[0].size() && !end; ++i)
                            if (transmitted[0][i] == 0xF7)
                                end = i + 1;
                        if (end)
                            break;
                        status = machine.run(Machine::kCpuHz / 1000);
                    }
                    const auto sent = sysex::decode(message);
                    const int command = sent && !sent->empty() ? (*sent)[0] : -1;
                    if (!end) {
                        ++missing;
                        replies += std::format(" [{:02x}:-]", command);
                        continue;
                    }
                    replies += std::format(" [{:02x}]", command);
                    ++acknowledged;
                    const std::vector<std::uint8_t> reply(transmitted[0].begin() + before, transmitted[0].begin() + end);
                    if (const auto payload = sysex::decode(reply); payload && !payload->empty())
                        for (std::size_t i = 0; i < payload->size() && ((*payload)[0] != 0x17 || i == 0); ++i)
                            replies += std::format("{}{:02x}", i ? "." : " ", (*payload)[i]);
                    else
                        replies += " ??";
                }
                std::printf("patch '%s' -> %c%03d: %d acknowledged, %d missing; replies:%s\n", patch.name.c_str(),
                            'A' + bankTo, (bankSlot >= 0 ? bankSlot : patch.slot) + 1, acknowledged, missing, replies.c_str());
                std::fflush(stdout);
            }
        }

        if (program >= 0 && status != Machine::Status::Fatal) {
            // Bank select (the LSB, CC 32, picks the bank), then program change, then let the firmware load it.
            machine.sendMidi({0xB0, 0x00, 0x00});
            machine.sendMidi({0xB0, 0x20, static_cast<std::uint8_t>(programBank)});
            machine.sendMidi({0xC0, static_cast<std::uint8_t>(program)});
            status = machine.run(2 * Machine::kCpuHz);
            std::printf("program change %d sent (bank %c)\n", program, 'A' + programBank);
        }

        if (!extraMidi.empty() && status != Machine::Status::Fatal) {
            machine.sendMidi(extraMidi);
            status = machine.run(Machine::kCpuHz / 10);
        }

        // HYDRA_GUEST_PROFILE: samples the main CPU's PC (every 50 us of wall time)
        // from here to the end of the note, and lists the hottest PCs and callers.
        // HYDRA_HOST_PROFILE: from here to the end of the note, where the main
        // thread's time goes (slices, JIT exits, device accesses).
        const bool hostProfile = std::getenv("HYDRA_HOST_PROFILE") != nullptr;
        const auto hostStart = std::chrono::steady_clock::now();
        const std::uint64_t tscStart = __rdtsc(), ticksStart = machine.cpu().ticksExecuted();
        const std::uint64_t idleStart = machine.idleTicks(), skippedStart = machine.cpu().ticksSkipped();
        const std::uint64_t svcStart = machine.cpu().svcCount, sysStart = machine.cpu().systemInstructions;
        const std::array<std::uint64_t, 32> irqStart = machine.intc().raisedCounts();
        const std::uint64_t panelStart = machine.panel().core().now(), panelIdleStart = machine.panel().core().idleTicks();
        machine.cpu().svcCycles = machine.cpu().systemInstructionCycles = machine.cpu().irqCycles = 0;
        machine.effects().flushes = machine.effects().flushNs = machine.effects().flushBacklog = 0;
        const std::uint64_t translatedStart = machine.cpu().blocksTranslated, panelTranslatedStart = machine.panel().core().blocksTranslated;
        if (hostProfile) {
            machine.profile() = Machine::Profile{};
            machine.profile().enabled = true;
        }
        machine.cpu().setProfileBlocks(std::getenv("HYDRA_BLOCK_PROFILE") != nullptr);
        machine.panel().core().setProfileBlocks(std::getenv("HYDRA_PANEL_BLOCK_PROFILE") != nullptr);
        std::atomic<bool> guestProfiling = std::getenv("HYDRA_GUEST_PROFILE") != nullptr;
        std::thread guestSampler;
        if (guestProfiling)
            guestSampler = std::thread([&] {
                while (guestProfiling.load()) {
                    machine.cpu().requestSample();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });

        if (waitSeconds > 0 && status != Machine::Status::Fatal)
            status = machine.run(static_cast<std::uint64_t>(waitSeconds * Machine::kCpuHz));

        // Panel actions, each followed by 300 ms: b<id> presses a button for 100 ms,
        // p<n> pad n (row * 8 + column) for 300 ms,
        // d<n> turns the data encoder, e<i>:<n> encoder i, w<ms> waits, s<file> writes
        // the displays (as HYDRA_SCREEN).
        std::istringstream actions(panelScript);
        for (std::string action; actions >> action && status != Machine::Status::Fatal;) {
            FrontPanel& front = machine.frontPanel();
            const int value = std::atoi(action.c_str() + 1);
            std::uint64_t ms = 300;
            switch (action[0]) {
            case 'b':
                front.setButton(value, true);
                status = machine.run(Machine::kCpuHz / 10);
                front.setButton(value, false);
                break;
            case 'd': front.turnDataEncoder(value); break;
            case 'p':  // pad row * 8 + column, held for 300 ms
                front.setPad(value / FrontPanel::kPadColumns, value % FrontPanel::kPadColumns, 0.8f);
                status = machine.run(Machine::kCpuHz * 3 / 10);
                front.setPad(value / FrontPanel::kPadColumns, value % FrontPanel::kPadColumns, 0.0f);
                break;
            case 'e':
                if (const auto colon = action.find(':'); colon != std::string::npos)
                    front.turnEncoder(value, std::atoi(action.c_str() + colon + 1));
                break;
            case 'k':  // k<knob>:<per mille>, e.g. k0:800 sets cutoff to 0.8
                if (const auto colon = action.find(':'); colon != std::string::npos)
                    front.setKnob(static_cast<FrontPanel::Knob>(value), static_cast<float>(std::atoi(action.c_str() + colon + 1)) / 1000.0f);
                break;
            case 'l': {  // the button LED bytes (main RAM 0x150E19) that changed since the last 'l'
                static std::vector<std::uint8_t> last(96, 0);
                std::string changes;
                for (std::uint32_t i = 0; i < 96; ++i) {
                    const auto v = static_cast<std::uint8_t>(machine.cpu().read32((0x150E19 + i) & ~3u) >> (8 * ((0x150E19 + i) & 3)));
                    if (v != last[i])
                        changes += std::format(" {}:{}>{}", i, last[i], v);
                    last[i] = v;
                }
                std::printf("button LEDs:%s\n", changes.c_str());
                ms = 0;
                break;
            }
            case 'w': ms = static_cast<std::uint64_t>(value); break;
            case 's':
                writeScreen(machine.panel(), action.substr(1));
                ms = 0;
                break;
            default: std::fprintf(stderr, "unknown panel action %s\n", action.c_str()); break;
            }
            status = machine.run(ms * Machine::kCpuHz / 1000);
            std::printf("panel action %s done\n", action.c_str());
        }

        if (note && status != Machine::Status::Fatal) {
            // Per sample: tone generator buses and DSP ports 64-71 (onSample), and the
            // audio output (onAudio, possibly on the effects thread); combined at the end.
            std::vector<std::uint32_t> portFrames;  // 56 words per sample
            std::vector<std::int32_t> audio;        // interleaved stereo, 24-bit range
            bool inNote = false;
            std::int64_t tgPeak = 0, dspPeak = 0;
            int maxVoices = 0;
            auto& tg = machine.toneGenerator();
            machine.flushEffects();
            machine.onAudio = [&](std::int32_t left, std::int32_t right) {
                audio.push_back(left);
                audio.push_back(right);
            };
            machine.onSample = [&](const std::uint32_t* ports) {
                portFrames.insert(portFrames.end(), ports, ports + 48);
                portFrames.insert(portFrames.end(), ports + 64, ports + 72);
                if (!inNote)
                    return;
                for (int i = 64; i < 72; ++i)
                    dspPeak = std::max<std::int64_t>(dspPeak, std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(ports[i]))));
                for (int i = 0; i < ToneGenerator::kBuses; ++i)
                    tgPeak = std::max<std::int64_t>(tgPeak, std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(ports[i]))));
                maxVoices = std::max(maxVoices, tg.activeVoices());
            };
            const std::uint64_t idleStart = machine.toneGenerator().dataWrites();
            machine.dspBlock().cpuWrites_.clear();
            status = machine.run(Machine::kCpuHz);
            std::printf("CPU writes to the DSP block in the quiet second:");
            for (const auto& [block, count] : machine.dspBlock().cpuWrites_)
                std::printf(" %05x:%llu", block, static_cast<unsigned long long>(count));
            std::printf("\n");
            machine.dspBlock().cpuWrites_.clear();
            std::printf("\nbaseline second without MIDI: %llu tone generator writes\n",
                        static_cast<unsigned long long>(machine.toneGenerator().dataWrites() - idleStart));
            const std::uint64_t tgBefore = machine.toneGenerator().dataWrites();
            std::array<std::size_t, 3> txBefore{};
            for (int i = 0; i < 3; ++i)
                txBefore[i] = transmitted[i].size();
            inNote = true;
            machine.dspBlock().commands_.clear();
            std::vector<std::uint32_t> before(0x2000);
            for (std::uint32_t i = 0; i < before.size(); ++i)
                before[i] = machine.dspBlock().read(0x20000 + 4 * i, 4);
            // Which io addresses does each core write during the note, and are the values non-zero?
            // One map per core: cores may run on different threads.
            std::array<std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>>, DspBlock::kCores> ioWrites;  // count, nonzero
            for (int c = 0; c < DspBlock::kCores; ++c)
                machine.dspBlock().core(c).onIoWrite = [&writes = ioWrites[c]](std::uint32_t a, std::uint32_t v) {
                    auto& e = writes[a & ~0xFu];
                    ++e.first;
                    e.second += v != 0;
                };
            // HYDRA_MAIN_LOG: also log main-bus accesses to modelled devices while the note plays.
            machine.bus().log().all = std::getenv("HYDRA_MAIN_LOG") != nullptr;
            // HYDRA_VOICE_DEBUG: follow each voice's amp envelope (firmware 2.2.0 addresses, see
            // docs/hardware.md) and the per-voice amp targets the CPU hands to the DSP.
            const bool voiceDebug = std::getenv("HYDRA_VOICE_DEBUG") != nullptr;
            constexpr std::uint32_t kEnvelope = 0x585080, kVoiceStride = 34472;  // voice 0's envelope 1
            if (voiceDebug && !watch) {
                machine.cpu().setWatch(kEnvelope, kEnvelope + 8 * kVoiceStride);
                // HYDRA_WATCH_VALUE (hex): only count writes of that value.
            const char* watchValue = std::getenv("HYDRA_WATCH_VALUE");
            const std::uint64_t wanted = watchValue ? std::strtoull(watchValue, nullptr, 16) : 0;
            machine.cpu().onWatchedWrite = [&, watchValue, wanted](std::uint32_t pc, std::uint32_t lr, std::uint32_t, std::uint64_t value) {
                if (watchValue && value != wanted)
                    return;
                auto& e = watchWriters[{pc, lr}];
                ++e.first;
                e.second = value;
            };
            }
            machine.sendMidi({0x90, 60, 100});
            for (int step = 0; step < 5; ++step) {
                status = machine.run(Machine::kCpuHz / 10);
                if (!voiceDebug)
                    continue;
                CpuCore& c = machine.cpu();
                std::printf("after %3d ms:\n", 100 * (step + 1));
                for (int v = 0; v < 8; ++v) {
                    const std::uint32_t env = kEnvelope + kVoiceStride * v;
                    std::printf("  voice %d env1: state %u phase %08x rates %08x %08x %08x %08x sustain %08x output %08x\n", v,
                                c.read32(env + 4 * 17), c.read32(env + 4 * 18), c.read32(env + 4 * 19), c.read32(env + 4 * 21),
                                c.read32(env + 4 * 22), c.read32(env + 4 * 28), c.read32(env + 4 * 23), c.read32(env + 4 * 33));
                }
                // Main pitch terms of each sounding voice (see docs/hardware.md, "Voice control").
                for (int v = 0; v < 8; ++v) {
                    const std::uint32_t v2 = 0x554B80 + 34472 * v, v3 = 0x58CB80 + 34472 * v;
                    if (c.read32(0x585080 + 34472 * v + 4 * 17) == 0)
                        continue;
                    const auto i32 = [&](std::uint32_t a) { return static_cast<std::int32_t>(c.read32(a)); };
                    std::printf("  voice %d pitch (1/256 semitone): note %d, global pitch mod %d, osc tune %d -> osc1 %d\n", v,
                                i32(v3 + 4 * 711), (i32(v3 + 4 * 650) - 0x2000) / 5, i32(v2 + 232052), i32(0x567994 + 1340 * v));
                }
                if (step == 0) {
                    // System settings struct; +636 is the master tune in Hz (see the factory settings in Machine).
                    const std::uint32_t settings = c.read32(0x47FA64), w = c.read32((settings + 636) & ~3u);
                    std::printf("  master tune %u Hz\n", ((settings + 636) & 2) ? w >> 16 : w & 0xFFFF);
                }
                // Smoothed controller slots (step, target, current; pitch bend first, centred at 0x2000).
                std::printf("  controllers 5800bc+12i (current):");
                for (int i = 0; i < 8; ++i)
                    std::printf(" %x", c.read32(0x5800C4 + 12 * i));
                std::printf("\n");
                // DSP voice parameter k is at shared io 0x8214 + 8k + lane; k = 37 is the amp target.
                std::printf("  amp targets (lanes 0-7):");
                for (int lane = 0; lane < 8; ++lane)
                    std::printf(" %08x", machine.dspBlock().read(0x20000 + 4 * (0x214 + 8 * 37 + lane), 4));
                std::printf("\n");
            }
            if (voiceDebug || watch) {
                machine.cpu().onWatchedWrite = nullptr;
                std::vector<std::pair<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>> writers;
                for (const auto& [where, e] : watchWriters)
                    writers.emplace_back(e.first, where);
                std::sort(writers.rbegin(), writers.rend());
                std::printf("busiest writers of %08x-%08x (block pc, lr, writes, last value):\n", machine.cpu().watchFrom(),
                            machine.cpu().watchTo());
                for (std::size_t i = 0; i < std::min<std::size_t>(writers.size(), 10); ++i)
                    std::printf("  %08x lr %08x x%llu last %llx\n", writers[i].second.first, writers[i].second.second,
                                static_cast<unsigned long long>(writers[i].first),
                                static_cast<unsigned long long>(watchWriters[writers[i].second].second));
            }
            std::printf("CPU writes to the DSP block during the note:");
            for (const auto& [block, count] : machine.dspBlock().cpuWrites_)
                std::printf(" %05x:%llu", block, static_cast<unsigned long long>(count));
            std::printf("\n");
            std::printf("DSP commands during the note:");
            for (const auto& [key, count] : machine.dspBlock().commands_)
                std::printf(" core%d:%x x%llu", key.first, key.second, static_cast<unsigned long long>(count));
            std::printf("\n");
            for (int c = 0; c < DspBlock::kCores; ++c)
                machine.dspBlock().core(c).onIoWrite = nullptr;
            int shown = 0;
            for (std::uint32_t i = 0; i < before.size() && shown < 120; ++i) {
                const std::uint32_t now = machine.dspBlock().read(0x20000 + 4 * i, 4);
                if (now != before[i]) {
                    std::printf("shared io %05x: %08x -> %08x\n", 0x8000 + i, before[i], now);
                    ++shown;
                }
            }
            for (int c = 0; c < DspBlock::kCores; ++c)
                for (const auto& [block, e] : ioWrites[c])
                    std::printf("io write core%d %05x-%05x: %llu writes, %llu non-zero\n", c, block, block + 15,
                                static_cast<unsigned long long>(e.first), static_cast<unsigned long long>(e.second));
            std::printf("voices while the note is held:\n%s", machine.toneGenerator().describeActiveVoices().c_str());
            machine.sendMidi({0x80, 60, 0});
            status = machine.run(Machine::kCpuHz / 2);
            machine.flushEffects();
            machine.onSample = nullptr;  // they refer to this block's locals
            machine.onAudio = nullptr;
            const std::size_t samples = std::min(portFrames.size() / 56, audio.size() / 2);
            std::int64_t outPeak = 0;
            for (std::size_t i = 44100 * 2; i < 2 * samples; ++i)
                outPeak = std::max<std::int64_t>(outPeak, std::abs(static_cast<std::int64_t>(audio[i])));
            if (capturePath) {
                if (std::FILE* capture = std::fopen(capturePath, "wb")) {
                    for (std::size_t i = 0; i < samples; ++i) {
                        std::fwrite(&portFrames[56 * i], 4, 56, capture);
                        std::fwrite(&audio[2 * i], 4, 2, capture);
                    }
                    std::fclose(capture);
                }
            }
            if (wavPath && !writeWav(wavPath, audio))
                std::fprintf(stderr, "cannot write %s\n", wavPath);
            std::printf("during the note: up to %d active voices, tone generator peak %lld, DSP output (ports 64-71) peak %lld, "
                        "audio output peak %lld\n",
                        maxVoices, static_cast<long long>(tgPeak), static_cast<long long>(dspPeak), static_cast<long long>(outPeak));
            std::printf("second with MIDI note on/off: %llu tone generator writes",
                        static_cast<unsigned long long>(machine.toneGenerator().dataWrites() - tgBefore));
            for (int i = 0; i < 3; ++i)
                std::printf(", uart%d sent %zu bytes", i, transmitted[i].size() - txBefore[i]);
            std::printf("\n");
        }
        if (const Machine::Profile& p = machine.profile(); p.enabled)
            std::printf("profile (wall s): main CPU %.2f, DSP cores %.2f, effects %.2f, tone generator %.2f, panel MCU %.2f, "
                        "waiting for the DSP thread %.2f\n",
                        p.cpu / 1e9, p.dspCores / 1e9, p.effects / 1e9, p.toneGenerator / 1e9, p.panel / 1e9, p.dspWait / 1e9);
        CpuCore& cpu = machine.cpu();

        const std::uint64_t elapsed = cpu.ticksExecuted();
        std::printf("\nstatus: %s after %.3f s emulated (%llu ticks, %.1f%% idle)\n",
                    status == Machine::Status::Fatal ? "FATAL" : "running",
                    double(elapsed) / Machine::kCpuHz, static_cast<unsigned long long>(elapsed),
                    elapsed ? 100.0 * double(machine.idleTicks()) / double(elapsed) : 0.0);
        std::printf("  %.1f%% of the CPU's time fast-forwarded in delay loops (%llu loop entries)\n",
                    elapsed ? 100.0 * double(cpu.ticksSkipped()) / double(elapsed) : 0.0,
                    static_cast<unsigned long long>(cpu.delayLoopEntries()));
        if (!cpu.fatalMessage().empty())
            std::printf("  %s\n", cpu.fatalMessage().c_str());

        const auto& r = cpu.regs();
        for (int i = 0; i < 16; ++i)
            std::printf("%sr%-2d=%08x", i % 4 ? "  " : "\n  ", i, r[i]);
        std::printf("\n  cpsr=%08x (mode %02x)  sctlr=%08x  vbar=%08x\n", cpu.cpsr(), cpu.cpsr() & 0x1F,
                    cpu.cp15().sctlr(), cpu.cp15().vbar());
        if (const auto code = cpu.MemoryReadCode(r[15]))
            std::printf("  next: %s\n", Dynarmic::A32::DisassembleArm(*code).c_str());

        for (int i = 0; i < 3; ++i) {
            std::printf("uart%d: rx %llu, tx %zu:", i, static_cast<unsigned long long>(machine.uart(i).bytesReceived()),
                        transmitted[i].size());
            for (std::size_t b = 0; b < std::min<std::size_t>(transmitted[i].size(), 48); ++b)
                std::printf(" %02x", transmitted[i][b]);
            std::printf("%s\n", transmitted[i].size() > 48 ? " ..." : "");
            if (transmitted[i].size() > 48) {  // and the last bytes
                std::printf("  last:");
                for (std::size_t b = std::max<std::size_t>(transmitted[i].size() - 48, 48); b < transmitted[i].size(); ++b)
                    std::printf(" %02x", transmitted[i][b]);
                std::printf("\n");
            }
        }

        std::printf("\ninterrupts raised:");
        for (int line = 0; line < 32; ++line)
            if (const auto count = machine.intc().raisedCounts()[line])
                std::printf("  line %d x%llu", line, static_cast<unsigned long long>(count));
        std::printf("\n");

        machine.flushEffects();
        for (int i = 0; i < 2; ++i) {
            const FxDsp& fx = machine.effects().dsp(i);
            std::printf("%s: %llu words written, started %llu times, %s, pc=%03x, sample interrupts %llu%s%s\n", fx.name().c_str(),
                        static_cast<unsigned long long>(fx.wordsWritten()), static_cast<unsigned long long>(fx.starts()),
                        fx.running() ? "running" : "stopped", fx.core().pc(),
                        static_cast<unsigned long long>(fx.core().interruptsTaken(2)), fx.core().trapped() ? ", TRAPPED: " : "",
                        fx.core().trapMessage().c_str());
        }
        if (const char* fxPath = std::getenv("HYDRA_FX_DUMP")) {
            // Program and data memory of both effects DSPs, 4096 words each.
            if (std::FILE* f = std::fopen(fxPath, "wb")) {
                for (int i = 0; i < 2; ++i) {
                    const FxDsp& fx = machine.effects().dsp(i);
                    std::fwrite(fx.program().data(), 4, fx.program().size(), f);
                    std::fwrite(fx.data().data(), 4, fx.data().size(), f);
                }
                std::fclose(f);
            }
        }
        const NorFlash& nor = machine.norFlash();
        std::printf("NOR flash: %llu words programmed, %llu sectors erased",
                    static_cast<unsigned long long>(nor.wordsProgrammed()), static_cast<unsigned long long>(nor.sectorsErased()));
        if (nor.dirtyFirst() <= nor.dirtyLast())
            std::printf(", changed %08x-%08x", 0x40000000 + nor.dirtyFirst(), 0x40000000 + nor.dirtyLast());
        std::printf("\n");
        if (hostProfile) {
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - hostStart).count();
            const double tscPerNs = static_cast<double>(__rdtsc() - tscStart) / (wall * 1e9);
            const double emulated = static_cast<double>(machine.cpu().ticksExecuted() - ticksStart) / Machine::kCpuHz;
            const Machine::Profile& p = machine.profile();
            const auto ms = [&](double ns) { return ns / 1e6 / emulated; };  // per emulated second
            std::printf("host profile over %.2f s emulated (%.2f s wall), per emulated second:\n", emulated, wall);
            std::printf("  main loop %.0f ms: CPU JIT %.0f ms, panel %.0f ms, DSP wait %.0f ms, rest %.0f ms\n", ms(p.loop), ms(p.cpu),
                        ms(p.panel), ms(p.dspWait), ms(static_cast<double>(p.loop - p.cpu - p.panel - p.dspWait)));
            std::printf("  slices %.0f (idle %.0f, delay-loop skips %.0f)\n", p.slices / emulated, p.idleSlices / emulated,
                        p.delaySkips / emulated);
            std::printf("  panel link: %.0f bytes, %.0f extra panel runs while the panel catches up (%.0f ms)\n",
                        p.linkBytes / emulated, p.linkWaitRuns / emulated, ms(p.linkWait));
            const double instructions = static_cast<double>(machine.cpu().ticksExecuted() - ticksStart -
                                                            (machine.idleTicks() - idleStart) - (machine.cpu().ticksSkipped() - skippedStart));
            const double jitRuns = static_cast<double>(p.slices - p.idleSlices - p.delaySkips);
            std::printf("  guest instructions %.0fM (%.2f ns each in the JIT), %.0f JIT runs (%.0f instructions each), SVCs %.0f, "
                        "system instructions %.0f\n",
                        instructions / 1e6 / emulated, static_cast<double>(p.cpu) / instructions, jitRuns / emulated, instructions / jitRuns,
                        (machine.cpu().svcCount - svcStart) / emulated, (machine.cpu().systemInstructions - sysStart) / emulated);
            std::printf("  C++ time per emulated second: SVC entry %.1f ms, IRQ entry %.1f ms, system instructions %.1f ms\n",
                        ms(machine.cpu().svcCycles / tscPerNs), ms(machine.cpu().irqCycles / tscPerNs),
                        ms(machine.cpu().systemInstructionCycles / tscPerNs));
            std::printf("  effects flushes (I2C reads): %.0f per second, waiting %.1f ms, worker %.0f samples behind on average\n",
                        machine.effects().flushes / emulated, ms(static_cast<double>(machine.effects().flushNs)),
                        machine.effects().flushes ? static_cast<double>(machine.effects().flushBacklog) / static_cast<double>(machine.effects().flushes) : 0.0);
            {
                MmioBus& pb = machine.panel().bus();
                std::printf("  panel ADC1: CR1 %08x CR2 %08x SMPR1 %08x SMPR2 %08x SQR1 %08x SQR2 %08x SQR3 %08x, RCC CFGR %08x\n",
                            pb.read(0x40012404, 4), pb.read(0x40012408, 4), pb.read(0x4001240C, 4), pb.read(0x40012410, 4),
                            pb.read(0x4001242C, 4), pb.read(0x40012430, 4), pb.read(0x40012434, 4), pb.read(0x40021004, 4));
            }
            std::printf("  blocks translated: main CPU %.0f, panel %.0f per second\n",
                        (machine.cpu().blocksTranslated - translatedStart) / emulated,
                        (machine.panel().core().blocksTranslated - panelTranslatedStart) / emulated);
            std::printf("  panel MCU: %.0f%% of its cycles idle\n",
                        100.0 * static_cast<double>(machine.panel().core().idleTicks() - panelIdleStart) /
                            static_cast<double>(machine.panel().core().now() - panelStart));
            std::printf("  main timer channels (control, period in 67.7 MHz ticks):");
            for (std::uint32_t ch = 0; ch < 6; ++ch)
                std::printf(" %u:%x/%u", ch, machine.bus().read(0xF1000000 + 0x10 * ch, 4), machine.bus().read(0xF1000004 + 0x10 * ch, 4));
            std::printf("\n");
            std::printf("  interrupts per line:");
            for (int line = 0; line < 32; ++line)
                if (const std::uint64_t n = machine.intc().raisedCounts()[line] - irqStart[line])
                    std::printf(" %d:%.0f", line, n / emulated);
            std::printf("\n");
            std::printf("  JIT exits by halt reason:");
            for (const auto& [reason, count] : machine.cpu().exitReasons())
                std::printf(" %x:%.0f", reason, count / emulated);
            std::printf("\n");
            for (MmioBus* bus : {&machine.bus(), &machine.panel().bus()}) {
                std::vector<std::pair<std::uint64_t, const Device*>> devices;
                for (const auto& [device, stats] : bus->stats())
                    devices.emplace_back(stats.cycles, device);
                std::sort(devices.rbegin(), devices.rend());
                std::printf("  %s device accesses (count, time):\n", bus == &machine.bus() ? "main" : "panel");
                for (std::size_t i = 0; i < std::min<std::size_t>(devices.size(), 12); ++i) {
                    const MmioBus::DeviceStats& st = bus->stats().at(devices[i].second);
                    std::printf("    %-12.*s %9.0f reads %9.0f writes %7.1f ms  (%.0f ns each)\n",
                                static_cast<int>(devices[i].second->name().size()), devices[i].second->name().data(),
                                st.reads / emulated, st.writes / emulated, ms(st.cycles / tscPerNs),
                                st.cycles / tscPerNs / static_cast<double>(st.reads + st.writes));
                }
            }
        }
        if (machine.panel().core().profileBlocks()) {
            machine.panel().core().setProfileBlocks(false);
            std::vector<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t, std::uint32_t>> blocks;
            double total = 0;
            for (const auto& [pc, entry] : machine.panel().core().blockCounts()) {
                blocks.emplace_back(entry.first * entry.second, pc, entry.first, entry.second);
                total += static_cast<double>(entry.first * entry.second);
            }
            std::sort(blocks.rbegin(), blocks.rend());
            std::printf("panel block profile: %.0fM instructions in %zu blocks (block start, instructions, entries x length):\n", total / 1e6, blocks.size());
            double cumulative = 0;
            for (std::size_t i = 0; i < std::min<std::size_t>(blocks.size(), 50); ++i) {
                const auto [instructions, pc, entries, length] = blocks[i];
                cumulative += static_cast<double>(instructions);
                std::printf("  %08x %5.1f%% (cum %5.1f%%)  %10llu x %3u\n", pc, 100.0 * static_cast<double>(instructions) / total,
                            100.0 * cumulative / total, static_cast<unsigned long long>(entries), length);
            }
        }
        if (machine.cpu().profileBlocks()) {
            // HYDRA_BLOCK_PROFILE: exact instruction counts per translated block,
            // from the end of the boot to the end of the note.
            std::vector<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t, std::uint32_t>> blocks;  // instructions, pc, entries, length
            double total = 0;
            for (const auto& [pc, entry] : machine.cpu().blockCounts()) {
                blocks.emplace_back(entry.first * entry.second, pc, entry.first, entry.second);
                total += static_cast<double>(entry.first * entry.second);
            }
            std::sort(blocks.rbegin(), blocks.rend());
            machine.cpu().setProfileBlocks(false);
            std::printf("block profile: %.0fM instructions in %zu blocks (block start, instructions, entries x length):\n", total / 1e6, blocks.size());
            double cumulative = 0;
            for (std::size_t i = 0; i < std::min<std::size_t>(blocks.size(), 60); ++i) {
                const auto [instructions, pc, entries, length] = blocks[i];
                cumulative += static_cast<double>(instructions);
                std::printf("  %08x %5.1f%% (cum %5.1f%%)  %10llu x %3u\n", pc, 100.0 * static_cast<double>(instructions) / total,
                            100.0 * cumulative / total, static_cast<unsigned long long>(entries), length);
            }
        }
        if (guestSampler.joinable()) {
            guestProfiling = false;
            guestSampler.join();
            std::map<std::uint32_t, std::uint64_t> byPc;
            std::uint64_t total = 0;
            for (const auto& [where, count] : machine.cpu().guestSamples()) {
                byPc[where.first] += count;
                total += count;
            }
            std::vector<std::pair<std::uint64_t, std::uint32_t>> hottest;
            for (const auto& [pc, count] : byPc)
                hottest.emplace_back(count, pc);
            std::sort(hottest.rbegin(), hottest.rend());
            std::printf("guest PC samples: %llu\n", static_cast<unsigned long long>(total));
            for (std::size_t i = 0; i < std::min<std::size_t>(hottest.size(), 40); ++i) {
                const auto [count, pc] = hottest[i];
                std::printf("  %08x %5.1f%%  callers:", pc, 100.0 * static_cast<double>(count) / static_cast<double>(total));
                std::vector<std::pair<std::uint64_t, std::uint32_t>> callers;
                for (const auto& [where, n] : machine.cpu().guestSamples())
                    if (where.first == pc)
                        callers.emplace_back(n, where.second);
                std::sort(callers.rbegin(), callers.rend());
                for (std::size_t j = 0; j < std::min<std::size_t>(callers.size(), 3); ++j)
                    std::printf(" %08x(%llu)", callers[j].second, static_cast<unsigned long long>(callers[j].first));
                std::printf("\n");
            }
        }
        if (const char* dramPath = std::getenv("HYDRA_DRAM_DUMP")) {
            if (std::FILE* f = std::fopen(dramPath, "wb")) {
                for (std::uint32_t a = 0; a < Machine::Config{}.dramSize; a += 4) {
                    const std::uint32_t w = cpu.read32(a);
                    std::fwrite(&w, 4, 1, f);
                }
                std::fclose(f);
            }
        }
        if (const char* norPath = std::getenv("HYDRA_NOR_DUMP")) {
            if (std::FILE* f = std::fopen(norPath, "wb")) {
                for (std::uint32_t a = 0x40000000; a < 0x48000000; a += 4) {
                    const std::uint32_t w = cpu.read32(a);
                    std::fwrite(&w, 4, 1, f);
                }
                std::fclose(f);
            }
        }
        PanelMcu& panel = machine.panel();
        if (const char* screenPath = std::getenv("HYDRA_SCREEN"))
            writeScreen(panel, screenPath);
        std::printf("panel GPIOB ODR %04x (PB10/PB11 -> main GPIO0 in bits 4/5)\n", panel.gpio(1).output());
        if (panelLog)
            for (const auto& [where, e] : panel.bus().log().entries)
                std::printf("  panel %-10s %08x %c %10llu  pc=%08x  %08x\n", e.device.c_str(), e.addr, e.write ? 'W' : 'R',
                            static_cast<unsigned long long>(e.count), e.firstPc, e.lastValue);
        std::printf("panel SPI2 overruns: %llu\n", static_cast<unsigned long long>(panel.spi(1).overruns));
        if (const char* peek = std::getenv("HYDRA_PANEL_PEEK")) {  // comma-separated hex addresses: panel RAM bytes
            std::printf("panel RAM:");
            for (const char* p = peek; *p;) {
                char* end;
                const auto addr = static_cast<std::uint32_t>(std::strtoul(p, &end, 16));
                std::printf(" [%08x]=%02x", addr, panel.core().read(addr, 1));
                p = *end == ',' ? end + 1 : end;
                if (end == p) break;
            }
            std::printf("\n");
        }
        std::printf("panel TIM2: CR1 %08x DIER %08x PSC %u ARR %u CCR1 %u CCR2 %u CCMR1 %08x; DMA1 ch1 CCR %08x CNDTR %u; ADC CR2 %08x\n",
                    panel.bus().read(0x40000000, 4), panel.bus().read(0x4000000C, 4), panel.bus().read(0x40000028, 4),
                    panel.bus().read(0x4000002C, 4), panel.bus().read(0x40000034, 4), panel.bus().read(0x40000038, 4),
                    panel.bus().read(0x40000018, 4), panel.bus().read(0x40020008, 4), panel.bus().read(0x4002000C, 4),
                    panel.bus().read(0x40012408, 4));
        std::printf("panel MCU: %llu cycles%s%s, SPI link %llu bytes, %llu replies other than ff\n",
                    static_cast<unsigned long long>(panel.core().now()),
                    panel.core().fatalMessage().empty() ? "" : ", FATAL: ", panel.core().fatalMessage().c_str(),
                    static_cast<unsigned long long>(machine.panelLinkBytes()), static_cast<unsigned long long>(machine.panelLinkReplies()));
        DspBlock& dsp = machine.dspBlock();
        if (const char* dumpPath = std::getenv("HYDRA_DSP_DUMP")) {
            // Program memory of all cores, 4096 words each.
            if (std::FILE* f = std::fopen(dumpPath, "wb")) {
                for (std::uint32_t offset = 0; offset < 0x20000; offset += 4) {
                    const std::uint32_t word = dsp.read(offset, 4);
                    std::fwrite(&word, 4, 1, f);
                }
                std::fclose(f);
            }
        }
        for (int core = 0; core < DspBlock::kCores; ++core)
            for (const auto& [port, count] : dsp.portWrites_[core])
                std::printf("  port write core%d port %02x x%llu (now %08x)\n", core, port, static_cast<unsigned long long>(count),
                            dsp.ports()[port]);
        std::printf("DSP commands (core:command x count):");
        for (const auto& [key, count] : dsp.commands_)
            std::printf(" %d:%x x%llu", key.first, key.second, static_cast<unsigned long long>(count));
        std::printf("\n");
        std::printf("DSP block: run mask %02x, %llu samples, %llu instructions\n", dsp.runMask(),
                    static_cast<unsigned long long>(dsp.sample()), static_cast<unsigned long long>(dsp.instructionsExecuted()));
        for (int i = 0; i < DspBlock::kCores; ++i) {
            dsp::DspCore& core = dsp.core(i);
            std::printf("  core%d irq1 x%llu irq2 x%llu inIrq=%d pending=%x\n", i, static_cast<unsigned long long>(core.interruptsTaken(1)),
                        static_cast<unsigned long long>(core.interruptsTaken(2)), core.inInterrupt(), core.pending());
            std::printf("  core%d pc=%03x mailbox %08x %08x %08x %08x  r15=%08x %s%s\n", i, core.pc(),
                        dsp.read(0x7FF80 - 0x80 * i, 4), dsp.read(0x7FF84 - 0x80 * i, 4), dsp.read(0x7FF88 - 0x80 * i, 4),
                        dsp.read(0x7FF8C - 0x80 * i, 4), core.regs()[15], core.trapped() ? "TRAPPED: " : "",
                        core.trapMessage().c_str());
        }
        for (int i = 0; i < 2; ++i)
            std::printf("i2c bus %c: %llu transfers to absent addresses\n", 'A' + i,
                        static_cast<unsigned long long>(machine.i2cBuses()[i].nackedAddresses()));

        std::printf("\nunknown CP15 accesses (opc1/CRn/CRm/opc2):\n");
        for (const auto& [key, count] : cpu.cp15().unknownAccesses())
            std::printf("  p15 %u c%u c%u %u  x%llu\n", key >> 12, key >> 8 & 0xF, key >> 4 & 0xF, key & 7,
                        static_cast<unsigned long long>(count));

        std::printf("\nperipheral accesses (address, R/W, count, first pc, last value):\n");
        for (const auto& [where, entry] : machine.bus().log().entries)
            std::printf("  %-8s %08x %c %10llu  pc=%08x  %08x\n", entry.device.c_str(), entry.addr, entry.write ? 'W' : 'R',
                        static_cast<unsigned long long>(entry.count), entry.firstPc, entry.lastValue);

        std::vector<std::pair<std::uint64_t, std::uint32_t>> hot;
        for (const auto& [pc, count] : machine.pcSamples())
            hot.emplace_back(count, pc);
        std::sort(hot.rbegin(), hot.rend());
        std::printf("\nhottest slice-end PCs:\n");
        for (std::size_t i = 0; i < std::min<std::size_t>(hot.size(), 10); ++i)
            std::printf("  %08x  x%llu\n", hot[i].second, static_cast<unsigned long long>(hot[i].first));
        return status == Machine::Status::Fatal ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
