#include "ogg_writer.h"

#include <cstdio>
#include <cstring>

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

namespace audio {

OGGWriter::OGGWriter() {}

OGGWriter::~OGGWriter() {
    if (outFile || streamState || vorbisState) {
        close();
    }
}

bool OGGWriter::open(const std::string &path, const AudioFileConfig &config) {
    if (outFile) {
        lastError = "File already open";
        return false;
    }

    outFile = std::fopen(path.c_str(), "wb");
    if (!outFile) {
        lastError = "Failed to open file: " + path;
        return false;
    }

    channels = config.channels;

    vorbisInfo = new vorbis_info;
    vorbisComment = new vorbis_comment;
    vorbisState = new vorbis_dsp_state;
    vorbisBlock = new vorbis_block;
    streamState = new ogg_stream_state;

    vorbis_info_init(vorbisInfo);

    // Initialize encoder with quality setting (0-10)
    // quality 6 = ~128 kbps per channel
    if (vorbis_encode_init_vbr(vorbisInfo, static_cast<long>(config.channels),
                                static_cast<long>(config.sampleRate),
                                static_cast<float>(quality) / 10.0f) != 0) {
        lastError = "Failed to initialize Vorbis encoder";
        return false;
    }

    vorbis_comment_init(vorbisComment);
    vorbis_comment_add_tag(vorbisComment, "ENCODER", "smaragd");

    if (vorbis_analysis_init(vorbisState, vorbisInfo) != 0) {
        lastError = "Failed to initialize Vorbis analysis state";
        return false;
    }

    if (vorbis_block_init(vorbisState, vorbisBlock) != 0) {
        lastError = "Failed to initialize Vorbis block";
        return false;
    }

    ogg_stream_init(streamState, 0);

    // Write Vorbis header packets
    ogg_packet headerMain, headerComment, headerCode;
    if (vorbis_analysis_headerout(vorbisState, vorbisComment, &headerMain,
                                   &headerComment, &headerCode) != 0) {
        lastError = "Failed to create Vorbis headers";
        return false;
    }

    ogg_stream_packetin(streamState, &headerMain);
    ogg_stream_packetin(streamState, &headerComment);
    ogg_stream_packetin(streamState, &headerCode);

    if (!flushPages(false)) {
        return false;
    }

    headerWritten = true;
    return true;
}

bool OGGWriter::write(const float *interleaved, std::size_t frameCount) {
    if (!outFile) {
        lastError = "File not open";
        return false;
    }

    // Get buffer from vorbis for writing
    float **buffer = vorbis_analysis_buffer(vorbisState, static_cast<int>(frameCount));

    // Deinterleave samples
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            buffer[ch][frame] = interleaved[frame * channels + ch];
        }
    }

    vorbis_analysis_wrote(vorbisState, static_cast<int>(frameCount));

    // Encode blocks
    while (vorbis_analysis_blockout(vorbisState, vorbisBlock) == 1) {
        vorbis_analysis(vorbisBlock, nullptr);
        vorbis_bitrate_addblock(vorbisBlock);

        ogg_packet packet;
        while (vorbis_bitrate_flushpacket(vorbisState, &packet)) {
            ogg_stream_packetin(streamState, &packet);
            if (!writePages()) {
                return false;
            }
        }
    }

    return true;
}

bool OGGWriter::close() {
    if (!outFile) {
        return true;
    }

    // Flush remaining data
    if (vorbisState) {
        vorbis_analysis_wrote(vorbisState, 0);

        while (vorbis_analysis_blockout(vorbisState, vorbisBlock) == 1) {
            vorbis_analysis(vorbisBlock, nullptr);
            vorbis_bitrate_addblock(vorbisBlock);

            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(vorbisState, &packet)) {
                ogg_stream_packetin(streamState, &packet);
            }
        }

        if (!flushPages(true)) {
            lastError = "Failed to flush final pages";
        }
    }

    // Clean up
    if (streamState) {
        ogg_stream_clear(streamState);
        delete streamState;
        streamState = nullptr;
    }

    if (vorbisBlock) {
        vorbis_block_clear(vorbisBlock);
        delete vorbisBlock;
        vorbisBlock = nullptr;
    }

    if (vorbisState) {
        vorbis_dsp_clear(vorbisState);
        delete vorbisState;
        vorbisState = nullptr;
    }

    if (vorbisComment) {
        vorbis_comment_clear(vorbisComment);
        delete vorbisComment;
        vorbisComment = nullptr;
    }

    if (vorbisInfo) {
        vorbis_info_clear(vorbisInfo);
        delete vorbisInfo;
        vorbisInfo = nullptr;
    }

    if (outFile) {
        std::fclose(outFile);
        outFile = nullptr;
    }

    return true;
}

const char *OGGWriter::errorMessage() const {
    return lastError.c_str();
}

void OGGWriter::setQuality(int q) {
    quality = q < 0 ? 0 : (q > 10 ? 10 : q);
}

bool OGGWriter::writePages() {
    ogg_page page;
    while (ogg_stream_pageout(streamState, &page)) {
        if (std::fwrite(page.header, 1, page.header_len, outFile) != page.header_len ||
            std::fwrite(page.body, 1, page.body_len, outFile) != page.body_len) {
            lastError = "Failed to write OGG page to file";
            return false;
        }
    }
    return true;
}

bool OGGWriter::flushPages(bool eos) {
    ogg_page page;
    while (ogg_stream_flush(streamState, &page)) {
        if (std::fwrite(page.header, 1, page.header_len, outFile) != page.header_len ||
            std::fwrite(page.body, 1, page.body_len, outFile) != page.body_len) {
            lastError = "Failed to write OGG page to file";
            return false;
        }
    }
    return true;
}

}  // namespace audio
