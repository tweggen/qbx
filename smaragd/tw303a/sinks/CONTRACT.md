# tw/sinks — CONTRACT

Purpose: audio destinations. AudioFileWriter (open/write/close) with WAV
(libsndfile), OGG (libvorbisenc) and MP3 (dlopen'd libmp3lame) writers, plus
the frame sinks (AudioSink interface, FileSink with futures-buffered writes,
PlaybackSink).

Public headers: audio_file_writer.h, audio_sink.h, file_sink.h,
playback_sink.h. wav/ogg/mp3_writer.h are PRIVATE (src/).

Depends on: tw/core (AudioFrame lives there — shared with playback).
libsndfile/ogg/vorbis are PRIVATE link deps. Forbidden: tw/playback (the
frame type moved to core precisely so sinks need not know the engine).

Invariants:
1. createAudioFileWriter(format) is the only factory; MP3 degrades
   gracefully when libmp3lame is absent (UI disables the option).
2. Writers are single-thread, non-realtime; input is interleaved float32 at
   the caller's stated rate.
3. FileSink::flush() must complete before close() — RenderSession relies on
   this ordering for complete files.

How to test: every render qxa case goes through WAVWriter + FileSink;
format coverage beyond WAV is manual (File -> Render...).

Known debt: no float-WAV/24-bit path exercised by tests; PlaybackSink is
minimally used.
