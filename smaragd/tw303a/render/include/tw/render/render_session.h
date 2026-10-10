#ifndef _RENDER_SESSION_H_
#define _RENDER_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tw/sinks/audio_file_writer.h"
#include "tw/playback/audio_engine.h"
#include "tw/sinks/file_sink.h"

class twComponent;
class CaptureRevalidator;

namespace audio {

// Whether a render with AudioFormat::MP3 can actually run. MP3 export dlopens
// libmp3lame rather than linking it, so this is a RUNTIME question, and the UI
// has to ask it before offering the format.
//
// Declared HERE, in render, rather than only in tw/sinks, because the app may
// include tw/render and may NOT include tw/sinks -- check_layering.py's edge
// set gives app/servicesui {core, devices, graph, playback, plugins, record,
// render}. srenderdialog.cpp used to hand-write its own
// `class MP3Writer { static bool isAvailable(); }` to get round exactly that,
// and the first attempt at this fix reached for tw/sinks directly and was
// correctly rejected by the checker.
//
// It also reads better: the dialog's business is a RENDER, not a file writer.
bool mp3ExportAvailable();

// The library names that were tried, comma-separated, so a failure can say what
// it looked for rather than telling the user to copy a file the build may
// already have deployed under another name.
std::string mp3LibraryCandidates();

struct RenderParams {
    enum class Extent { EntireProject, TimeSelection };

    Extent extent = Extent::EntireProject;
    double startTimeSec = 0.0;
    double endTimeSec = 0.0;
    AudioFormat format = AudioFormat::WAV;
    int quality = 6;  // 0-10 for OGG
    std::string outputPath;

    // Channels in the OUTPUT FILE (proposal 36 B5). This was hard-coded 2 in
    // RenderSession::start, with the graph's single mono page duplicated into
    // both — the render half of the mono sink.
    //
    // 0 means "ask the graph", which is what every caller should normally want:
    // the root component's declared width IS the project's width (SStdMixer
    // takes it from SProject::channels()), so deriving it cannot drift from the
    // project the way a copied number can. A caller may still pin a number; a
    // pinned count that exceeds the graph's width gets the §4.4 clamp per
    // channel ("mono plays on every channel"), and one below it drops the
    // channels above — the same rule twSpeaker states for a device.
    int channels = 0;
};

class RenderSession {
public:
    RenderSession();
    ~RenderSession();

    // Start rendering. Returns false if already rendering or invalid params.
    bool start(std::shared_ptr<twComponent> synthOutput, const RenderParams &params, std::uint32_t sampleRate);

    // Proposal 19 dataflow stage 4 — render as a WATERMARK CONSUMER. When a
    // scheduler is set (before start()), the render thread no longer drives
    // the freeze itself: it issues one full-range demand up front (the whole
    // page DAG, with predecessor chaining for exact sequential DSP state) and
    // then, per page, a single-page demand it WAITS on — the only blocking in
    // the pipeline, at the very edge — before reading the page from the
    // component cache. The caller must NOT pause the revalidator around such
    // a render (the demand executes on its worker pool); coordination is the
    // scheduler itself. Null (default) keeps the legacy sequential pull.
    void setScheduler(CaptureRevalidator *scheduler) { scheduler_ = scheduler; }

    // Proposal 40 M1b, AC 3: BOUNCE MODE for the per-page pruning. Empty
    // (default) keeps the export path's existing behaviour byte- and
    // behaviour-unchanged — the page-boundary release below still calls
    // twComponent::releaseOldPagesGlobally(), which prunes EVERY live
    // component in the process. A non-empty scope switches to per-component
    // twComponent::releaseOldPages() over exactly these components instead:
    // the global walk would prune a concurrently PLAYING graph's page trail
    // at the BOUNCER's position, which is a graph the bounce has no business
    // touching. Held as weak_ptr because the bounce does not own the track's
    // chain components; a component that outlived the bounce's caller is
    // simply skipped (lock() returns null) rather than kept alive by this.
    void setPruneScope(std::vector<std::weak_ptr<twComponent>> scope) {
        pruneScope_ = std::move(scope);
    }

    // Where start() gets its file writer. Unset (the default) is
    // createAudioFileWriter(params.format), which is what every production
    // caller wants. It exists so a test can hand the session a writer that
    // FAILS mid-stream (QBX-145): there is no portable way to make a real disk
    // fail on cue, and the failure path is exactly what was never exercised.
    using WriterFactory = std::function<std::unique_ptr<AudioFileWriter>(AudioFormat)>;
    void setWriterFactory(WriterFactory factory) { writerFactory_ = std::move(factory); }

    // Request cancellation. Safe to call from any thread.
    void requestCancel();

    // Query state (safe to call from any thread)
    bool isRunning() const;
    std::size_t samplesWritten() const;
    std::size_t totalSamples() const;
    const char *errorMessage() const;
    // The outcome of the LAST render that ran to the end of its thread. It is
    // stored before running_ is cleared, so a poller that sees isRunning()
    // turn false reads the outcome of that render, not a stale one -- the
    // ordering is spelled out as an atomic rather than left implicit in the
    // store order of a std::string and an atomic<bool> (QBX-145). A UI
    // dialog POLLS this; it must not take over onComplete (THREADING.md
    // rule 1), which belongs to whoever called start().
    bool lastSuccess() const;
    // The rate start() was given; valid once start() has returned true.
    std::uint32_t sampleRate() const { return sampleRate_; }

    // Callbacks (called from render thread, must be thread-safe)
    std::function<void(std::size_t written, std::size_t total)> onProgress;
    std::function<void(bool success, const char *error)> onComplete;
    // Playhead publication: absolute project position in frames, so the app
    // can move its locator along with the render. Called from the render
    // thread — the handler must be realtime-safe (atomic store, no Qt/UI).
    // Replaces the former direct SApplication call (proposal 14, Phase 0).
    std::function<void(std::uint64_t absPos)> onPosition;

private:
    void renderThreadMain();

    std::shared_ptr<twComponent> synthOutput_;
    RenderParams params_;
    std::uint32_t sampleRate_ = 48000;
    unsigned renderChannels_ = 1;   // resolved in start(); what the file carries
    std::size_t totalSamples_ = 0;
    std::size_t startOffsetSamples_ = 0;
    std::atomic<std::size_t> samplesWritten_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> lastSuccess_{false};
    std::unique_ptr<std::thread> renderThread_;
    std::string lastError_;
    std::unique_ptr<AudioFileWriter> writer_;
    std::unique_ptr<FileSink> fileSink_;        // Buffered output with futures
    CaptureRevalidator *scheduler_ = nullptr;   // Stage 4: borrowed, optional
    std::vector<std::weak_ptr<twComponent>> pruneScope_;   // M1b bounce mode
    WriterFactory writerFactory_;               // QBX-145: test seam, normally unset
};

}  // namespace audio

#endif
