# tw/sinks — CONTRACT

Purpose: audio destinations. AudioFileWriter (open/write/close) with WAV
(libsndfile), OGG (libvorbisenc) and MP3 (dlopen'd libmp3lame) writers, plus
the block sinks (AudioSink interface, FileSink with futures-buffered writes,
PlaybackSink).

> **Correction (2026-10-10, QBX-145):** MP3 is no longer a dlopen'd libmp3lame.
> That writer's open() failed unconditionally, so MP3 export had never worked.
> WAV and MP3 are now one libsndfile-backed `SndfileWriter` (src/), MP3 as
> `SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III` at a constant bitrate; LAME is
> inside libsndfile's `mpeg` feature, the same feature MP3 import needs. OGG is
> unchanged (libvorbisenc).

Public headers: audio_file_writer.h, audio_sink.h, file_sink.h,
playback_sink.h. wav/ogg/mp3_writer.h are PRIVATE (src/). *(2026-10-10,
QBX-145: now sndfile_writer.h, ogg_writer.h and utf8_path.h.)*

Depends on: tw/core. libsndfile/ogg/vorbis are PRIVATE link deps. Forbidden:
tw/playback (nothing here knows the engine).

Invariants:
1. createAudioFileWriter(format) is the only factory; MP3 degrades
   gracefully when libmp3lame is absent (UI disables the option).
   **Corrected 2026-10-10 (QBX-145):** there is no libmp3lame to be absent.
   MP3 is available when `mp3EncoderAvailable(rate, channels, &reason)` says
   so: one or two channels, an MPEG sample rate (8000, 11025, 12000, 16000,
   22050, 24000, 32000, 44100, 48000), and a libsndfile that can open an MPEG
   encoder -- probed with `sf_open_virtual` on a discard file, because
   `sf_format_check` accepts MPEG whether or not the encoder was built. The
   writer applies the same checks at open() (one function, `mp3ConfigError`,
   serves both), so the dialog can never offer what the writer refuses. The
   writer neither resamples nor folds channels. The UI disables the option
   with the reason as its tooltip.
2. Writers are single-thread, non-realtime; input is interleaved float32 at
   the caller's stated rate.
3. FileSink::flush() must complete before close() — RenderSession relies on
   this ordering for complete files.
4. A SINK IS N-CHANNEL AND THE WIDTH TRAVELS WITH THE CALL (proposal 36 B5):
   `writeFrames(interleaved, nFrames, channels)`, one call per block. It used
   to be `writeFrame(const AudioFrame&)`, one call per FRAME, and AudioFrame's
   `float channels[MAX_CHANNELS]` with `MAX_CHANNELS == 2` was the hard stereo
   cap in tw/core that made the sink the last mono thing in the engine. That
   type is deleted; there is no fixed-width frame currency any more.
5. WRITERS TAKE THEIR QUALITY BEFORE open() (QBX-145).
   `AudioFileWriter::setQuality(int)` defaults to a no-op; MP3 reads it as
   kbps (<= 10 means the default 192, otherwise clamped to 128..320), OGG as
   Vorbis quality 0..10, WAV ignores it (16-bit PCM). MP3 must set the
   compression level and THEN constant-bitrate mode before the first write:
   libsndfile ignores the level after data, and the mode then fails with
   SFE_CMD_HAS_DATA. Never `sf_set_string` on an MP3: any string makes
   libsndfile write an ID3v2 tag, and tw/sources invariant 14 is the mpg123
   hang a bare sf_open of such a tag causes on Windows.
6. A FAILED WRITE IS STICKY (QBX-145). FileSink used to drop
   `writer->write()`'s result. The first failure now records the writer's
   message (`hasWriteError()`, `writeError()`), no later block reaches the
   writer, and `writeFrames()` returns false from then on. RenderSession
   fails the render with that message after `flush()`. No Qt, no new thread:
   the flag lives under FileSink's existing buffer mutex.
7. PATHS ARE UTF-8 (QBX-145). The app hands `QString::toStdString()`. On
   Windows the writers widen it (`utf8_path.h`) and open with `_wfopen` (OGG)
   or `sf_wchar_open` (WAV, MP3): this build's libsndfile `sf_open` reads a
   narrow path in the ANSI code page, measured, as std::fopen does.

How to test: every render qxa case goes through WAVWriter + FileSink;
format coverage beyond WAV is manual (File -> Render...).
*(2026-10-10, QBX-145: no longer manual for MP3 and OGG.)* `ctest -R
mp3_writer_test` writes a sine to MP3 and reads it back through libsndfile
(length within one frame, RMS within 5%), checks the >2-channel and 96 kHz
refusals and that the probe agrees, that the bitrate reaches the encoder, and
on Windows that WAV/OGG/MP3 all open a non-ANSI UTF-8 path. `render_test`
renders MP3 end to end, proves OGG quality changes the file, and proves a
failing write fails the render. `qxa.render_mp3_export` renders an
arrangement to MP3 and asserts length and energy against the WAV reference.

Known debt: no float-WAV/24-bit path exercised by tests; PlaybackSink is
minimally used. The render dialog's WAV bit-depth box still reaches nothing:
WAV stays 16-bit PCM whatever it says (QBX-145 deliberately left it).
