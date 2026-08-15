// asio_driver_list — see the header for why this is SDK-free by design.

#include "asio_driver_list.h"

#if defined( _WIN32 )

#include <windows.h>

#include <combaseapi.h>

namespace audio {

namespace {

// A registry string value, or empty. REG_SZ only — an ASIO installer that
// writes anything else wrote it wrong, and we would rather skip the driver
// than misread it.
std::string regString( HKEY key, const char *valueName )
{
    DWORD type = 0;
    DWORD bytes = 0;
    if( RegQueryValueExA( key, valueName, nullptr, &type, nullptr, &bytes ) != ERROR_SUCCESS )
        return std::string();
    if( type != REG_SZ || bytes == 0 )
        return std::string();

    std::string out( bytes, '\0' );
    if( RegQueryValueExA( key, valueName, nullptr, nullptr,
                          reinterpret_cast<LPBYTE>( &out[0] ), &bytes ) != ERROR_SUCCESS )
        return std::string();
    // bytes includes the terminator if the installer wrote one; trim any
    // trailing NULs either way.
    while( !out.empty() && out.back() == '\0' )
        out.pop_back();
    return out;
}

bool clsidParses( const std::string &s )
{
    if( s.empty() )
        return false;
    // CLSIDFromString wants a wide string; a CLSID is pure ASCII, so widening
    // by value is exact.
    std::wstring w( s.begin(), s.end() );
    CLSID clsid;
    return CLSIDFromString( w.c_str(), &clsid ) == NOERROR;
}

}  // namespace

std::vector<AsioDriverEntry> scanAsioDrivers()
{
    std::vector<AsioDriverEntry> out;

    // 64-bit view explicitly: the app is x64-only (proposal 35), and a 32-bit
    // driver registered only in the WOW64 view could not be loaded in-process
    // anyway, so listing it would be a lie.
    HKEY root = nullptr;
    if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0,
                       KEY_READ | KEY_WOW64_64KEY, &root ) != ERROR_SUCCESS )
        return out;

    for( DWORD i = 0;; ++i ) {
        char name[256];
        DWORD nameLen = sizeof( name );
        const LSTATUS rc =
            RegEnumKeyExA( root, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr );
        if( rc == ERROR_NO_MORE_ITEMS )
            break;
        if( rc != ERROR_SUCCESS )
            continue;

        HKEY sub = nullptr;
        if( RegOpenKeyExA( root, name, 0, KEY_READ | KEY_WOW64_64KEY, &sub ) != ERROR_SUCCESS )
            continue;

        AsioDriverEntry e;
        e.name = name;
        e.clsid = regString( sub, "CLSID" );
        e.description = regString( sub, "Description" );
        RegCloseKey( sub );

        if( e.description.empty() )
            e.description = e.name;
        if( clsidParses( e.clsid ) )
            out.push_back( std::move( e ) );
    }

    RegCloseKey( root );
    return out;
}

}  // namespace audio

#else  // !_WIN32

namespace audio {

std::vector<AsioDriverEntry> scanAsioDrivers()
{
    return {};
}

}  // namespace audio

#endif
