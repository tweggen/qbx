#include "app/testkit/sassertrecordmodeaction.h"
#include "app/objects/track/srecordmode.h"
#include "app/objects/track/strack.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDebug>
#include <QDomElement>

using namespace strackpath;

SApplyResult SAssertRecordModeAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        qWarning() << "assert-record-mode: no root named" << pathRoot_;
        return { false, nullptr };
    }

    if( !project_.isEmpty() ) {
        const QString got = srecmode::projectMode( project );
        if( got != project_ ) {
            qWarning() << "assert-record-mode FAILED: project mode expected"
                       << project_ << "got" << got;
            return { false, nullptr };
        }
    }

    // trackPath is optional: a case may assert the project mode alone.
    if( resolved_.isEmpty() && !haveOwn_ ) return { true, nullptr };

    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "assert-record-mode: no lane at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }

    if( haveOwn_ ) {
        const STrack *t = dynamic_cast<const STrack *>( lane );
        const QString got = t ? t->getRecordMode() : QString();
        if( got != own_ ) {
            qWarning() << "assert-record-mode FAILED: own mode at"
                       << qualifiedToString( pathRoot_, trackPath_ )
                       << "expected" << ( own_.isEmpty() ? "<inherit>" : own_ )
                       << "got" << ( got.isEmpty() ? "<inherit>" : got );
            return { false, nullptr };
        }
    }
    if( !resolved_.isEmpty() ) {
        const QString got = srecmode::resolveMode( mixer, lane, project );
        if( got != resolved_ ) {
            qWarning() << "assert-record-mode FAILED: resolved mode at"
                       << qualifiedToString( pathRoot_, trackPath_ )
                       << "expected" << resolved_ << "got" << got;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertRecordModeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    if( !resolved_.isEmpty() ) elem.setAttribute( "resolved", resolved_ );
    if( haveOwn_ )             elem.setAttribute( "own", own_ );
    if( !project_.isEmpty() )  elem.setAttribute( "project", project_ );
}

bool SAssertRecordModeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath", "0" ) );
    resolved_  = elem.attribute( "resolved", "" );
    project_   = elem.attribute( "project", "" );
    // `own` is checked when PRESENT, because its interesting value is the
    // EMPTY string (inherit) -- "absent" and "asserted empty" cannot be the
    // same thing here.
    haveOwn_   = elem.hasAttribute( "own" );
    own_       = elem.attribute( "own", "" );
    return true;
}

static const bool s_reg_assertrecordmode = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-record-mode"),
        []{ return new SAssertRecordModeAction; }
    ), true
);
