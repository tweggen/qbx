#include "app/servicesui/soptions.h"

#include "tw/plugins/twpluginsearchpaths.h"

#include <QStringList>

QVariant SOpt::def( const QString &key )
{
    // Pan (no Cmd) vs. Zoom (with Cmd); Vertical (no Shift) vs. Horiz (with Shift)
    if( key == WheelPlain )     return (int) ScrollVertical;
    if( key == WheelShift )     return (int) ScrollHorizontal;
    if( key == WheelCtrl )      return (int) ZoomVertical;
    if( key == WheelCtrlShift ) return (int) ZoomHorizontal;
    if( key == ZoomToCursor )   return true;
    if( key == InvertZoom )     return false;
    // 100 % is not merely "the middle of the range": it is the value at which
    // every wheel gesture reduces to exactly the arithmetic it had before this
    // option existed, so a user who never opens the dialog sees no change.
    if( key == WheelSensitivityPct ) return 100;
    if( key == FollowPlayhead ) return true;
    if( key == AudioDeviceId )  return QString();

    // The console default is the build's, so a Debug build keeps its console
    // without the user configuring anything and a Release build stays quiet.
    if( key == LogConsole )     return SMARAGD_LOG_CONSOLE_DEFAULT ? true : false;
    if( key == LogLevel )       return QStringLiteral( "debug" );
    if( key == LogCapacity )    return 200000;
    if( key == LogToFile )      return true;

    if( key == ShortcutClipProperties ) return QStringLiteral( "F2" );

    // The only platform-dependent default in this table, and deliberately not
    // spelled out here: the per-OS plugin locations belong to the engine's
    // twPluginSearchPaths, so the scanner and the options page can never
    // disagree about where plugins live.
    // Every hosted format contributes its own standard locations; the list is one
    // flat set of directories because the scanner keys on the FILE EXTENSION, not
    // on which default produced the directory. Order matters only for display.
    if( key == PluginSearchPaths ) {
        QStringList dirs;
        for( const char *fmt : { "clap", "vst3" } )
            for( const std::string &d : audio::twPluginSearchPaths::defaults( fmt ) )
                dirs << QString::fromStdString( d );
        dirs.removeDuplicates();
        return dirs;
    }
    if( key == PluginScanOnStartup ) return true;

    // MIDI (proposal 37 P7b). Note-on chase is OFF by default for MIDI OUT -
    // see the key's comment; controller chase is not optional.
    if( key == MidiOutOffsetMs )  return 0;
    if( key == MidiChaseNoteOns ) return false;
    if( key == MidiInputPortIds ) return QStringList();
    // MIDI recording (proposal 21 L4). New-take because it is the only mode
    // that cannot destroy what was already played; quantise OFF because a
    // performance the user did not ask to have quantised must arrive as played.
    if( key == MidiRecordMode )     return QStringLiteral( "new-take" );
    if( key == MidiRecordQuantize ) return QStringLiteral( "off" );
    // Transport polish (proposal 21 L5). Both bar counts are 0 because a
    // count-in or a pre-roll nobody asked for delays every take; the click
    // level is a comfortable -6 dBFS accent, with the ordinary beat at 0.7 of
    // it (twmetronome.h's "2 kHz square / 1 kHz square, 1.0 / 0.7"). Click
    // while recording defaults ON so the metronome behaves exactly as before
    // this option existed.
    if( key == MetronomeLevel )      return 0.5;
    if( key == CountInBars )         return 0;
    if( key == PreRollBars )         return 0;
    if( key == ClickWhileRecording ) return true;
    // Media browser (proposal 38 gate 2). "local" is the only source the MVP
    // registers, so remembering it is the honest default rather than an empty
    // string the combo would have to interpret. The mask is Audio (1) --
    // smedia::Category::Audio, spelled as the literal because app/servicesui
    // may not include app/media and a second include for one bit would be a
    // layering edge bought for nothing.
    if( key == MediaLastSourceId )    return QStringLiteral( "local" );
    if( key == MediaLastPath )        return QString();
    if( key == MediaCategoryMask )    return 1;
    if( key == MediaSearchRecursive ) return false;
    // Gate 3's cache cap (§B.6). 2 GB is a sample library's worth of downloads
    // and small enough that a cache nobody prunes cannot fill a disk.
    if( key == MediaCacheCapMB )      return 2048;
    return QVariant();
}

QString SOpt::wheelActionLabel( WheelAction a )
{
    switch( a ) {
    case None:             return "Do nothing";
    case ScrollVertical:   return "Scroll tracks (vertical)";
    case ScrollHorizontal: return "Scroll timeline (horizontal)";
    case ZoomHorizontal:   return "Zoom horizontal";
    case ZoomVertical:     return "Zoom vertical";
    }
    return "Do nothing";
}
