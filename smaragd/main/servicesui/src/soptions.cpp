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
