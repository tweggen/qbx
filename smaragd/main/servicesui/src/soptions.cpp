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
    if( key == PluginSearchPaths ) {
        QStringList dirs;
        for( const std::string &d : audio::twPluginSearchPaths::defaults( "clap" ) )
            dirs << QString::fromStdString( d );
        return dirs;
    }
    if( key == PluginScanOnStartup ) return true;
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
