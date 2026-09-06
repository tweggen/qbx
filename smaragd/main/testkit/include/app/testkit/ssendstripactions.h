#ifndef SSENDSTRIPACTIONS_H
#define SSENDSTRIPACTIONS_H

#include "app/actions/saction.h"

/**
 * `assert-send-strip` / `send-strip-set` — the Track Detail dock's Sends
 * section, built OFF SCREEN over the real panel (proposal 47 M5).
 *
 * **`send-strip-set` MOVES A REAL CONTROL**, which is the difference between
 * gating the verb and gating the WIRING. A test that called the handler
 * directly would pass with the checkbox connected to nothing. This one sets
 * the widget and lets Qt deliver the signal, so a missing `connect()` fails
 * — the one class of UI defect this repo can actually gate, alongside
 * geometry.
 *
 * What is still NOT gated, and is hand-verified: that the section is visible
 * where a user expects it, and how it looks. `screenshot` grabs a root window
 * that is blank under `QT_QPA_PLATFORM=offscreen`, so pixels are out of reach
 * — the standing gap `assert-track-head` and `assert-track-detail-layout`
 * already work around by building one widget and measuring geometry.
 */
class SAssertSendStripAction : public SAction {
public:
    SAssertSendStripAction() = default;
    QString name() const override { return QStringLiteral( "assert-send-strip" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "rows" ),
               QStringLiteral( "contains" ), QStringLiteral( "absent" ) }; }
private:
    QString trackPath_;
    QString contains_;
    QString absent_;
    int     rows_ = -1;
};

class SSendStripSetAction : public SAction {
public:
    SSendStripSetAction() = default;
    QString name() const override { return QStringLiteral( "send-strip-set" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "lane" ),
               QStringLiteral( "control" ), QStringLiteral( "value" ) }; }
private:
    QString trackPath_;
    QString lane_;
    QString control_;
    double  value_ = 0.0;
};

#endif
