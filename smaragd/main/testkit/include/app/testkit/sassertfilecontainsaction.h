#ifndef SASSERTFILECONTAINSACTION_H
#define SASSERTFILECONTAINSACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: a text file contains (or does not contain) a literal string.
 *
 * Added for proposal 08 M4, to gate what a rendered WAV cannot show: that a
 * SAVED PROJECT still carries a plugin slot's descriptor and its opaque state
 * chunk verbatim — including for a plugin that is not installed on this machine,
 * where losing the blob would silently destroy the user's settings.
 *
 * XML format:
 *   <assert-file-contains path="../../build/x.qxp" text="uid='foo'"/>
 *   <assert-file-contains path="../../build/x.qxp" text="bar" absent="true"/>
 *
 * Parameters:
 * - path:   file to read, relative to the PROCESS working directory — the same
 *           convention save-project/load-project use (ctest runs qxa cases from
 *           tests/cases/). Absolute paths work too.
 * - text:   literal substring (not a regex) that must be present.
 * - absent: "true" inverts the check.
 */
class SAssertFileContainsAction : public SAction {
public:
    SAssertFileContainsAction() = default;
    SAssertFileContainsAction( const QString &path, const QString &text,
                               bool absent = false );

    QString name() const override { return QStringLiteral("assert-file-contains"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString path_;
    QString text_;
    bool    absent_ = false;
};

#endif
