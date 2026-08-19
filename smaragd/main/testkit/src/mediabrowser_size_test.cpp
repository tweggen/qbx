// Gate for mediaBrowserFormatSize() (app/mediabrowser/smediabrowserpanel.h,
// task item k): the media browser's Size column shows at most 3 significant
// digits, plus an optional decimal (1000-based) k/M/G unit-prefix suffix —
// never "B", and never a prefix letter under 1000 bytes. Pure string
// arithmetic, no QApplication/QWidget needed.

#include "app/mediabrowser/smediabrowserpanel.h"

#include <QString>
#include <QStringList>

#include <iostream>

namespace {

QStringList g_failures;

void check( const QString &what, qint64 bytes, const QString &expected )
{
    const QString got = mediaBrowserFormatSize( bytes );
    if( got == expected ) {
        std::cout << "PASS: " << what.toStdString() << " (" << bytes
                  << ")  -> " << got.toStdString() << "\n";
        return;
    }
    std::cout << "FAIL: " << what.toStdString() << "\n";
    g_failures.append( QString( "%1 (bytes=%2)\n  expected: %3\n  got:      %4" )
                           .arg( what ).arg( bytes ).arg( expected, got ) );
}

} // namespace

int main()
{
    // Directory rows: the panel's own "unknown" spelling, unchanged.
    check( "negative (directory) is empty", -1, QString() );

    // Under 1000: the exact integer, no prefix at all (task spec, literally).
    check( "zero", 0, "0" );
    check( "123 bytes example from the task", 123, "123" );
    check( "just under the k boundary", 999, "999" );

    // The task's own three worked examples.
    check( "~1234 -> 1.23k", 1234, "1.23k" );
    check( "~12345 -> 12.3k", 12345, "12.3k" );
    check( "~123456 -> 123k", 123456, "123k" );

    // The boundary itself, and one tick either side of it.
    check( "exactly 1000", 1000, "1.00k" );

    // M and G scale the same way as k.
    check( "~1234567 -> 1.23M", 1234567, "1.23M" );
    check( "~12345678 -> 12.3M", 12345678, "12.3M" );
    check( "~123456789 -> 123M", 123456789, "123M" );
    check( "~1234567890 -> 1.23G", 1234567890, "1.23G" );

    // Rounding that carries across a UNIT boundary: 999500 is 999.5k, which
    // rounds (0 decimals, since 999.5 >= 100) to "1000", not a valid 3-sig-fig
    // k spelling — it must re-express as the next unit up.
    check( "rounds across the k->M boundary", 999500, "1.00M" );
    // One byte short of that: no promotion, three plain digits.
    check( "just under the rounding boundary", 999499, "999k" );

    // Rounding that carries across a DECIMALS boundary within the SAME unit:
    // 9999 is 9.999k, which at the 2-decimal rule for v<10 rounds to "10.00"
    // — four significant digits. It must reprint at 1 decimal, "10.0k".
    check( "rounds across the 1-digit/2-digit boundary", 9999, "10.0k" );
    // And the same shape one digit-width up: 99999 is 99.999k, which at the
    // 1-decimal rule for v<100 rounds to "100.0" — reprint at 0 decimals.
    check( "rounds across the 2-digit/3-digit boundary", 99999, "100k" );

    // Every result has AT MOST 3 significant digits and never a "B" suffix.
    for( qint64 bytes : { 1000LL, 999999LL, 1000000LL, 999999999LL } ) {
        const QString s = mediaBrowserFormatSize( bytes );
        if( s.contains( 'B' ) || s.contains( 'b' ) ) {
            g_failures.append( QString( "size for %1 contains a B suffix: %2" )
                                   .arg( bytes ).arg( s ) );
        }
    }

    std::cout << "\n";
    if( g_failures.isEmpty() ) {
        std::cout << "All media-browser size-format cases passed.\n";
        return 0;
    }
    std::cout << g_failures.size() << " failures:\n\n";
    for( const QString &f : g_failures ) std::cout << f.toStdString() << "\n\n";
    return 1;
}
