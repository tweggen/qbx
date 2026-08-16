# tw/sinks — CONTRACT

Purpose: audio destinations. AudioFileWriter (open/write/close) with WAV
(libsndfile), OGG (libvorbisenc) and MP3 (dlopen'd libmp3lame) writers, plus
the block sinks (AudioSink interface, FileSink with futures-buffered writes,
PlaybackSink).

Public headers: audio_file_writer.h, audio_sink.h, file_sink.h,
playback_sink.h. wav/ogg/mp3_writer.h are PRIVATE (src/).

Depends on: tw/core. libsndfile/ogg/vorbis are PRIVATE link deps. Forbidden:
tw/playback (nothing here knows the engine).

Invariants:
1. createAudioFileWriter(format) is the only factory; MP3 degrades
   gracefully when libmp3lame is absent (UI disables the option).
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

How to test: every render qxa case goes through WAVWriter + FileSink;
format coverage beyond WAV is manual (File -> Render...).

Known debt: no float-WAV/24-bit path exercised by tests; PlaybackSink is
minimally used.
