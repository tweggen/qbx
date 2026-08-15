#include "app/testkit/sreportpagememoryaction.h"
#include "app/actions/sactionregistry.h"

#include "app/model/sproject.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/pages/tw_page_accounting.h"

#include <QDebug>
#include <QDomElement>

SReportPageMemoryAction::SReportPageMemoryAction( const QString &label,
                                                  long long maxPages,
                                                  long long maxBytes )
    : label_( label ), maxPages_( maxPages ), maxBytes_( maxBytes )
{
}

SApplyResult SReportPageMemoryAction::apply( SProject *project )
{
    const QByteArray label = label_.toUtf8();

    // ONE walk of the component registry. The engine writes the breakdown into
    // the log sink itself (TW_LOG, category "pages"), so it reaches the ring, the
    // rotating file and the log dock like every other engine diagnostic, and
    // hands the same text back — which then goes to qDebug too, so the figures
    // also land in the harness's own stdout beside the assertions they belong to.
    // A number that lives only in a log file is not a number anyone reads after a
    // test run; two independently-walked numbers would be worse than that.
    const std::string text = twComponent::reportPageMemory( label.constData() );
    for( const QString &line : QString::fromStdString( text ).split( '\n', Qt::SkipEmptyParts ) ) {
        qDebug().noquote() << "SReportPageMemoryAction:" << line;
    }

    // The capture pool's OCCUPANCY, which only the project can answer (the
    // engine-side counter knows what was reserved, not what is in use). Worth
    // one line: SProject reserves 2048 pages ~ 540 MB up front, and a report
    // that showed only the twOutputPage figures would understate this process's
    // page memory by a factor of forty.
    if( project && project->getPagePool() ) {
        CapturePagePool *pool = project->getPagePool();
        qDebug().noquote()
            << "SReportPageMemoryAction: page-memory [" + label_ + "]   capturePool"
            << "pages=" << pool->numPages()
            << "allocated=" << pool->numAllocatedPages()
            << "free=" << pool->numFreePages()
            << "bytesPerPage=" << (qulonglong) sizeof( CapturePageData );
    }

    const tw::pages::PageMemoryStats g = tw::pages::PageAccounting::global();

    if( maxPages_ >= 0 && (long long) g.pages > maxPages_ ) {
        qWarning() << "SReportPageMemoryAction:" << label_
                   << "- resident pages" << (qulonglong) g.pages
                   << "exceeds maxPages" << maxPages_;
        return { false, nullptr };
    }
    if( maxBytes_ >= 0 && (long long) g.bytes > maxBytes_ ) {
        qWarning() << "SReportPageMemoryAction:" << label_
                   << "- resident page bytes" << (qulonglong) g.bytes
                   << "exceeds maxBytes" << maxBytes_;
        return { false, nullptr };
    }

    return { true, nullptr };   // a report is not undoable
}

void SReportPageMemoryAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "label", label_ );
    if( maxPages_ >= 0 ) elem.setAttribute( "maxPages", QString::number( maxPages_ ) );
    if( maxBytes_ >= 0 ) elem.setAttribute( "maxBytes", QString::number( maxBytes_ ) );
}

bool SReportPageMemoryAction::readXml( const QDomElement &elem, int /*version*/ )
{
    label_ = elem.attribute( "label", "" );

    bool ok1 = true, ok2 = true;
    maxPages_ = elem.attribute( "maxPages", "-1" ).toLongLong( &ok1 );
    maxBytes_ = elem.attribute( "maxBytes", "-1" ).toLongLong( &ok2 );
    if( !ok1 || !ok2 ) {
        qWarning() << "SReportPageMemoryAction::readXml: invalid numeric attributes";
        return false;
    }
    return true;
}

static const bool s_reg_report_page_memory = (
    SActionRegistry::instance().registerType(
        QStringLiteral("report-page-memory"),
        []{ return new SReportPageMemoryAction; }
    ), true
);
