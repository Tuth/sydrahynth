#include "HydraProcessor.h"

#include <cmath>
#include <cstdlib>

#include "HydraEditor.h"

namespace {
constexpr int kStateVersion = 1;

juce::PropertiesFile::Options settingsOptions() {
    juce::PropertiesFile::Options options;
#if JUCE_WINDOWS
    // JUCE's standalone wrapper already owns Sydrahynth/Sydrahynth.settings.
    // Windows paths ignore case, so reserve distinct processor filenames too.
    options.applicationName = juce::JUCEApplicationBase::isStandaloneApp() ? "sydrahynth-standalone" : "sydrahynth-vst3";
    options.folderName = "sydrahynth";
#else
    options.applicationName = juce::JUCEApplicationBase::isStandaloneApp() ? "sydrahynth-standalone" : "sydrahynth";
    options.folderName = ".config/sydrahynth";  // on Linux relative to the home directory
#endif
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    return options;
}

// System settings for a fresh instrument: program change reception on, so the
// host's program changes select patches.
std::map<std::pair<int, int>, std::int16_t> initialSystemSettings() { return {{{5, 7}, 1}}; }
}  // namespace

HydraProcessor::HydraProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    settings_.setStorageParameters(settingsOptions());
    for (std::size_t i = 0; i < ccmap::kEntries.size(); ++i) {
        const auto& entry = ccmap::kEntries[i];
        auto* parameter = new juce::AudioParameterInt(juce::ParameterID{"cc" + juce::String(entry.cc), 1},
                                                      juce::String(entry.name) + " (CC " + juce::String(entry.cc) + ")", 0, 127, 0);
        ccParameters_[i] = parameter;
        addParameter(parameter);
        parameter->addListener(this);
    }
    outputGain_ = new juce::AudioParameterFloat(juce::ParameterID{"outputGainDb", 1}, "Output Gain",
                                                juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f,
                                                juce::AudioParameterFloatAttributes().withLabel("dB"));
    addParameter(outputGain_);
}

HydraProcessor::~HydraProcessor() {
    const std::lock_guard life(lifeMutex_);
    std::unique_ptr<hydra::Emulator> emulator;
    {
        const juce::SpinLock::ScopedLockType lock(emulatorLock_);
        emulator = std::move(emulator_);
    }
}

bool HydraProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

juce::File HydraProcessor::firmwareFile() const {
    auto& self = const_cast<HydraProcessor&>(*this);
    const juce::String stored = self.settings_.getUserSettings()->getValue("firmware");
    if (stored.isNotEmpty())
        return juce::File(stored);
    const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable("HYDRA_FIRMWARE", {});
    if (fromEnvironment.isNotEmpty())
        return juce::File(fromEnvironment);
    const juce::File folder =
#if JUCE_WINDOWS
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("sydrahynth/Firmware");
#else
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/sydrahynth");
#endif
    const auto found = folder.findChildFiles(juce::File::findFiles, false, "*.dat");
    return found.isEmpty() ? juce::File() : found.getFirst();
}

void HydraProcessor::setFirmwareFile(const juce::File& file) {
    const std::lock_guard life(lifeMutex_);
    settings_.getUserSettings()->setValue("firmware", file.getFullPathName());
    settings_.saveIfNeeded();
    std::vector<hydra::Machine::Config::FlashBlock> flash;
    if (emulator_ && emulator_->state() == hydra::Emulator::State::Running)
        flash = emulator_->captureFlash();
    restartEmulator(flash.empty() ? pendingFlash_ : std::move(flash));
}

void HydraProcessor::restartEmulator(std::vector<hydra::Machine::Config::FlashBlock> flash) {
    const std::lock_guard life(lifeMutex_);
    pendingFlash_ = flash;
    hydra::Emulator::Config config;
    config.firmwarePath = std::filesystem::u8path(firmwareFile().getFullPathName().toStdString());
    config.flash = std::move(flash);
    config.systemSettings = initialSystemSettings();
    config.targetFill = targetFill();
    auto next = std::make_unique<hydra::Emulator>();
    next->start(config);  // without a firmware file it fails, with a status saying so
    std::unique_ptr<hydra::Emulator> previous;
    {
        const juce::SpinLock::ScopedLockType lock(emulatorLock_);
        previous = std::move(emulator_);
        emulator_ = std::move(next);
    }
    previous.reset();  // stops its threads, outside the lock
}

void HydraProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    ratio_ = hydra::Emulator::kSampleRate / sampleRate;
    for (auto& resampler : resamplers_)
        resampler.reset();
    const auto capacity = static_cast<std::size_t>(std::ceil(4 * samplesPerBlock * ratio_) + 64);
    for (auto& fifo : fifo_)
        fifo.assign(capacity, 0.0f);
    fifoCount_ = 0;
    scratchLeft_.assign(capacity, 0.0f);
    scratchRight_.assign(capacity, 0.0f);
    blockSize_ = samplesPerBlock;
    {
        const std::lock_guard life(lifeMutex_);
        if (!started_) {
            started_ = true;
            restartEmulator(pendingFlash_);
        }
    }
    applyLatency();
}

int HydraProcessor::latencyChoice() const {
    if (const char* forced = std::getenv("HYDRA_LATENCY"))  // for tests: 0-3
        return juce::jlimit(0, static_cast<int>(kLatencyFrames.size()) - 1, std::atoi(forced));
    auto& self = const_cast<HydraProcessor&>(*this);
    return juce::jlimit(0, static_cast<int>(kLatencyFrames.size()) - 1, self.settings_.getUserSettings()->getIntValue("latency", 1));
}

bool HydraProcessor::browserVisible() const {
    auto& self = const_cast<HydraProcessor&>(*this);
    return self.settings_.getUserSettings()->getBoolValue("browserVisible", true);
}

void HydraProcessor::setBrowserVisible(bool visible) {
    const std::lock_guard life(lifeMutex_);
    settings_.getUserSettings()->setValue("browserVisible", visible);
    settings_.saveIfNeeded();
}

void HydraProcessor::setLatencyChoice(int choice) {
    {
        const std::lock_guard life(lifeMutex_);
        settings_.getUserSettings()->setValue("latency", choice);
        settings_.saveIfNeeded();
    }
    applyLatency();
}

std::size_t HydraProcessor::targetFill() const {
    const int block = static_cast<int>(std::ceil(blockSize_ * ratio_));
    return static_cast<std::size_t>(std::max(kLatencyFrames[static_cast<std::size_t>(latencyChoice())], block + 128));
}

void HydraProcessor::applyLatency() {
    const std::size_t fill = targetFill();
    {
        const std::lock_guard life(lifeMutex_);
        if (emulator_)
            emulator_->setTargetFill(fill);
    }
    setLatencySamples(static_cast<int>(std::lround(static_cast<double>(fill) / ratio_)));
}

void HydraProcessor::parameterValueChanged(int index, float) {
    if (!restoringState_ && index >= 0 && static_cast<std::size_t>(index) < ccDirty_.size())
        ccDirty_[static_cast<std::size_t>(index)] = true;
}

void HydraProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const juce::SpinLock::ScopedTryLockType lock(emulatorLock_);
    if (!lock.isLocked() || !emulator_)
        return;
    hydra::Emulator& emulator = *emulator_;

    for (const auto metadata : midi) {
        const juce::MidiMessage message = metadata.getMessage();
        emulator.sendMidi(message.getRawData(), static_cast<std::size_t>(message.getRawDataSize()));
    }
    for (std::size_t i = 0; i < ccParameters_.size(); ++i)
        if (ccDirty_[i].exchange(false)) {
            const std::uint8_t cc[3] = {0xB0, ccmap::kEntries[i].cc, static_cast<std::uint8_t>(ccParameters_[i]->get())};
            emulator.sendMidi(cc, 3);
        }

    pullAudio(buffer, emulator);
    buffer.applyGain(juce::Decibels::decibelsToGain(outputGain_->get()));
}

void HydraProcessor::pullAudio(juce::AudioBuffer<float>& buffer, hydra::Emulator& emulator) {
    const int frames = buffer.getNumSamples();
    float* left = buffer.getWritePointer(0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : scratchRight_.data();
    if (std::abs(ratio_ - 1.0) < 1e-9) {  // 44.1 kHz host: no resampling
        emulator.readAudio(left, right, static_cast<std::size_t>(frames));
        return;
    }
    const int needed = static_cast<int>(std::ceil(frames * ratio_)) + 2;
    if (needed > static_cast<int>(fifo_[0].size()))
        return;  // a block far larger than announced: skip rather than allocate here
    if (fifoCount_ < needed) {
        emulator.readAudio(fifo_[0].data() + fifoCount_, fifo_[1].data() + fifoCount_,
                           static_cast<std::size_t>(needed - fifoCount_));
        fifoCount_ = needed;
    }
    const int used = resamplers_[0].process(ratio_, fifo_[0].data(), left, frames);
    resamplers_[1].process(ratio_, fifo_[1].data(), right, frames);
    const int remaining = std::max(0, fifoCount_ - used);
    for (auto& fifo : fifo_)
        std::memmove(fifo.data(), fifo.data() + used, static_cast<std::size_t>(remaining) * sizeof(float));
    fifoCount_ = remaining;
}

void HydraProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree state("Sydrahynth");
    state.setProperty("version", kStateVersion, nullptr);
    for (std::size_t i = 0; i < ccParameters_.size(); ++i)
        state.setProperty("cc" + juce::String(ccmap::kEntries[i].cc), ccParameters_[i]->get(), nullptr);
    state.setProperty("outputGainDb", outputGain_->get(), nullptr);

    const std::lock_guard life(lifeMutex_);
    std::vector<hydra::Machine::Config::FlashBlock> flash;
    if (emulator_ && emulator_->state() == hydra::Emulator::State::Running)
        flash = emulator_->captureFlash();
    if (flash.empty())
        flash = pendingFlash_;
    for (const auto& block : flash) {
        juce::MemoryOutputStream compressed;
        {
            juce::GZIPCompressorOutputStream gzip(compressed, 9);
            gzip.write(block.bytes.data(), block.bytes.size());
        }
        juce::ValueTree child("flash");
        child.setProperty("offset", static_cast<juce::int64>(block.offset), nullptr);
        child.setProperty("size", static_cast<juce::int64>(block.bytes.size()), nullptr);
        child.setProperty("data", compressed.getMemoryBlock().toBase64Encoding(), nullptr);
        state.appendChild(child, nullptr);
    }
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void HydraProcessor::setStateInformation(const void* data, int sizeInBytes) {
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml)
        return;
    const juce::ValueTree state = juce::ValueTree::fromXml(*xml);
    if (!state.hasType("Sydrahynth"))
        return;

    restoringState_ = true;
    for (std::size_t i = 0; i < ccParameters_.size(); ++i) {
        const juce::var value = state.getProperty("cc" + juce::String(ccmap::kEntries[i].cc));
        if (!value.isVoid())
            *ccParameters_[i] = static_cast<int>(value);
    }
    if (const juce::var gain = state.getProperty("outputGainDb"); !gain.isVoid())
        *outputGain_ = static_cast<float>(gain);
    restoringState_ = false;

    std::vector<hydra::Machine::Config::FlashBlock> flash;
    for (const auto& child : state) {
        if (!child.hasType("flash"))
            continue;
        juce::MemoryBlock compressed;
        if (!compressed.fromBase64Encoding(child.getProperty("data").toString()))
            continue;
        juce::MemoryInputStream input(compressed, false);
        juce::GZIPDecompressorInputStream gzip(input);
        hydra::Machine::Config::FlashBlock block;
        block.offset = static_cast<std::uint32_t>(static_cast<juce::int64>(child.getProperty("offset")));
        block.bytes.resize(static_cast<std::size_t>(static_cast<juce::int64>(child.getProperty("size"))));
        if (gzip.read(block.bytes.data(), static_cast<int>(block.bytes.size())) != static_cast<int>(block.bytes.size()))
            continue;
        flash.push_back(std::move(block));
    }
    const std::lock_guard life(lifeMutex_);
    if (started_)
        restartEmulator(std::move(flash));
    else
        pendingFlash_ = std::move(flash);
}

juce::String HydraProcessor::statusText() const {
    const std::lock_guard life(lifeMutex_);
    if (!emulator_)
        return "not started";
    const hydra::Emulator& emulator = *emulator_;
    if (emulator.uploading()) {
        const auto [done, total] = emulator.uploadProgress();
        return "loading patches: " + juce::String(static_cast<int>(done)) + " of " + juce::String(static_cast<int>(total));
    }
    return juce::String(emulator.status()) + " | latency " +
           juce::String(1000.0 * static_cast<double>(emulator.targetFill()) / hydra::Emulator::kSampleRate, 1) +
           " ms | underruns " + juce::String(static_cast<juce::int64>(emulator.underruns()));
}

juce::String HydraProcessor::loadBank(const juce::File& file, int bank) {
    std::vector<hydra::Patch> patches;
    try {
        patches = hydra::loadPatches(file.getFullPathName().toStdString());
    } catch (const std::exception& e) {
        return "Cannot read " + file.getFileName() + ": " + e.what();
    }
    if (patches.empty())
        return file.getFileName() + " contains no patches";
    const std::lock_guard life(lifeMutex_);
    if (!emulator_ || !emulator_->uploadPatches(std::move(patches), bank))
        return "The instrument is not ready (still starting, or already receiving patches)";
    return {};
}

bool HydraProcessor::readPatches(hydra::Emulator::Patches& out, std::uint64_t& generation) const {
    const std::lock_guard life(lifeMutex_);
    return emulator_ && emulator_->readPatches(out, generation);
}

void HydraProcessor::selectPatch(int bank, int slot) {
    const std::lock_guard life(lifeMutex_);
    if (emulator_)
        emulator_->selectPatch(bank, slot);
}

void HydraProcessor::panelInput(const hydra::Emulator::PanelInput& input) {
    const std::lock_guard life(lifeMutex_);
    if (emulator_)
        emulator_->panelInput(input);
}

bool HydraProcessor::readPanel(hydra::FrontPanel::Lights& out, std::uint64_t& generation) const {
    const std::lock_guard life(lifeMutex_);
    return emulator_ && emulator_->readPanel(out, generation);
}

juce::AudioProcessorEditor* HydraProcessor::createEditor() { return new HydraEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HydraProcessor(); }
