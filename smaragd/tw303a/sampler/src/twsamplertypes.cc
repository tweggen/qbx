// twSamplerProgram / twSamplerSample: thin, immutable wrappers around the
// NassauDigital core's Program and Sample (proposal 56 §4.2).

#include "twsamplerimpl.h"

#include "nd_program_xml.h"

#include <atomic>
#include <vector>

namespace audio {

namespace {
std::atomic<std::uint64_t> g_programSerial{ 0 };
}

// --------------------------------------------------------------- program ---

twSamplerProgram::twSamplerProgram() : impl_( new Impl ) {}
twSamplerProgram::~twSamplerProgram() = default;

std::shared_ptr<const twSamplerProgram> twSamplerProgram::fromXml( const std::string &xml,
                                                                    std::string *error )
{
    nd::ProgramParseResult r = nd::parseProgram( xml );
    if( !r.ok ) {
        if( error ) *error = r.error;
        return nullptr;
    }
    std::shared_ptr<twSamplerProgram> p( new twSamplerProgram );
    p->impl_->program = std::make_shared<const nd::Program>( std::move( r.program ) );
    p->serial_        = g_programSerial.fetch_add( 1, std::memory_order_relaxed ) + 1;
    return p;
}

std::string twSamplerProgram::toXml() const
{
    return impl_->program ? nd::writeProgram( *impl_->program ) : std::string();
}

std::size_t twSamplerProgram::zoneCount() const
{
    return impl_->program ? impl_->program->zones.size() : 0u;
}

// ---------------------------------------------------------------- sample ---

twSamplerSample::twSamplerSample() : impl_( new Impl ) {}
twSamplerSample::~twSamplerSample() = default;

std::shared_ptr<const twSamplerSample> twSamplerSample::fromPlanar( int channels,
                                                                    double sampleRate,
                                                                    const float *const *planar,
                                                                    std::int64_t frames,
                                                                    std::uint64_t generation,
                                                                    std::string *error )
{
    if( channels != 1 && channels != 2 ) {
        if( error ) *error = "a sample has 1 or 2 channels";
        return nullptr;
    }
    if( frames > 0 && !planar ) {
        if( error ) *error = "no sample data";
        return nullptr;
    }
    // The core stores interleaved frames with its own padding; one copy here,
    // off the audio thread, is the whole cost of a planar host.
    std::vector<float> inter;
    if( frames > 0 ) {
        inter.resize( (std::size_t)frames * (std::size_t)channels );
        for( int c = 0; c < channels; ++c ) {
            const float *src = planar[c];
            if( !src ) {
                if( error ) *error = "a sample channel has no data";
                return nullptr;
            }
            for( std::int64_t i = 0; i < frames; ++i )
                inter[(std::size_t)i * (std::size_t)channels + (std::size_t)c] = src[i];
        }
    }
    std::shared_ptr<const nd::Sample> s = nd::Sample::fromInterleaved(
        channels, sampleRate, inter.empty() ? nullptr : inter.data(), frames, generation );
    if( !s ) {
        if( error ) *error = "the core refused the sample's shape (rate <= 0 or negative length)";
        return nullptr;
    }
    std::shared_ptr<twSamplerSample> out( new twSamplerSample );
    out->impl_->sample = std::move( s );
    return out;
}

int           twSamplerSample::channels() const   { return impl_->sample->channels(); }
double        twSamplerSample::sampleRate() const { return impl_->sample->sampleRate(); }
std::int64_t  twSamplerSample::frames() const     { return impl_->sample->frames(); }
std::uint64_t twSamplerSample::generation() const { return impl_->sample->generation(); }

}  // namespace audio
