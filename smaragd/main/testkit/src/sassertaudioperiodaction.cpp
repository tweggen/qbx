#include "app/testkit/sassertaudioperiodaction.h"

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/testkit/stestfilepath.h"

#include "tw/analysis/audio_analysis.h"

#include <QDebug>
#include <QDomElement>

#include <cmath>

QStringList SAssertAudioPeriodAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ),   QStringLiteral( "channel" ),
             QStringLiteral( "startFrame" ), QStringLiteral( "frameCount" ),
             QStringLiteral( "minMs" ),      QStringLiteral( "maxMs" ),
             QStringLiteral( "expectMs" ),   QStringLiteral( "toleranceMs" ) };
}

SApplyResult SAssertAudioPeriodAction::apply( SProject *project )
{
    const QString where = QStringLiteral( "assert-audio-period: %1" ).arg( filename_ );
    if( filename_.isEmpty() || !( maxMs_ > minMs_ ) || minMs_ <= 0.0 || toleranceMs_ < 0.0 ) {
        qWarning().noquote() << where
                             << ": needs filename, 0 < minMs < maxMs, expectMs and toleranceMs >= 0; "
                                "REJECTED";
        return { false, nullptr };
    }
    const QString outputDir = SApplication::app().testOutputDir();
    if( outputDir.isEmpty() ) {
        qWarning().noquote() << where << ": no test output directory configured";
        return { false, nullptr };
    }
    const QString path = resolveTestFilePath( filename_, outputDir, project );
    // The rate is the file's: read it first through the same reader.
    audio::AudioFileInfo info;
    std::string error;
    if( !audio::readAudioFileInfo( path.toStdString(), info, error ) ) {
        qWarning().noquote() << where << "FAIL:" << QString::fromStdString( error );
        return { false, nullptr };
    }
    const double rate = (double)info.sampleRate;
    const qint64 minLag = (qint64)std::floor( minMs_ * rate / 1000.0 );
    const qint64 maxLag = (qint64)std::ceil( maxMs_ * rate / 1000.0 );
    int  sr     = 0;
    bool onEdge = false;
    const double frames = audio::estimatePeriodFrames( path.toStdString(), startFrame_, frameCount_,
                                                       channel_, minLag, maxLag, sr, onEdge, error );
    if( !error.empty() || frames <= 0.0 ) {
        qWarning().noquote() << where << "FAIL:" << QString::fromStdString( error );
        return { false, nullptr };
    }
    const double ms  = frames / rate * 1000.0;
    const double off = ms - expectMs_;
    const QString got = QStringLiteral( "period %1 frames = %2 ms (expected %3 +- %4 ms, off by "
                                        "%5 ms; lags %6..%7)" )
                            .arg( frames, 0, 'f', 4 ).arg( ms, 0, 'f', 4 ).arg( expectMs_ )
                            .arg( toleranceMs_ ).arg( off, 0, 'f', 4 ).arg( minLag ).arg( maxLag );
    if( onEdge ) {
        qWarning().noquote() << where << "FAIL: the maximum is on the lag range's edge;" << got;
        return { false, nullptr };
    }
    if( !( std::fabs( off ) <= toleranceMs_ ) ) {
        qWarning().noquote() << where << "FAIL:" << got;
        return { false, nullptr };
    }
    qInfo().noquote() << where << ": OK," << got;
    return { true, nullptr };
}

void SAssertAudioPeriodAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    elem.setAttribute( "minMs", QString::number( minMs_, 'g', 12 ) );
    elem.setAttribute( "maxMs", QString::number( maxMs_, 'g', 12 ) );
    elem.setAttribute( "expectMs", QString::number( expectMs_, 'g', 12 ) );
    elem.setAttribute( "toleranceMs", QString::number( toleranceMs_, 'g', 12 ) );
}

bool SAssertAudioPeriodAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // A missing required attribute is refused in apply(): readXml must survive
    // a DEFAULT-CONSTRUCTED round trip (action_roundtrip_test).
    filename_    = elem.attribute( "filename" );
    channel_     = elem.attribute( "channel", "0" ).toInt();
    startFrame_  = elem.attribute( "startFrame", "0" ).toLongLong();
    frameCount_  = elem.attribute( "frameCount", "-1" ).toLongLong();
    minMs_       = elem.attribute( "minMs", "0" ).toDouble();
    maxMs_       = elem.attribute( "maxMs", "0" ).toDouble();
    expectMs_    = elem.attribute( "expectMs", "0" ).toDouble();
    toleranceMs_ = elem.attribute( "toleranceMs", "-1" ).toDouble();
    return true;
}

static const bool s_reg_assert_audio_period =
    ( SActionRegistry::instance().registerType( QStringLiteral( "assert-audio-period" ),
                                               [] { return new SAssertAudioPeriodAction; } ),
      true );
