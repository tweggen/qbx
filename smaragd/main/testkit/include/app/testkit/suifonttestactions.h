#ifndef _SUIFONTTESTACTIONS_H_
#define _SUIFONTTESTACTIONS_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * QBX-102 verbs: the two named UI fonts and the Track Detail dock's
 * collapsible sections. Each builds the REAL widget off screen through an
 * SMainWindow seam (testkit may not include app/timeline, app/mixerui or
 * app/mediabrowser), so what runs is the production constructor and, for the
 * section click, the production signal.
 */

/**
 * `assert-ui-fonts` — `contains` / `absent` against
 * SMainWindow::describeUiFonts( trackPath ):
 *   small=<pt>|tree=<pt>, then per mount m in head|strip|plugin|extern|media
 *   m=<pt>|m{Small|Tree}=<0|1>|mSet=<0|1>, plus stripEqHead=<0|1>
 * Small/Tree are EXACT QFont equality with suifonts::smallFont() / treeFont(),
 * which is the claim the request makes ("the same font as"). mSet is
 * Qt::WA_SetFont: the mount chose its font, which is what keeps the Tree
 * relation from passing by default where treeFont() is the app font. The
 * tree font's POINT SIZE is the platform theme's, so a case asserts the
 * relation, never that number.
 */
class SAssertUiFontsAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-ui-fonts" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
private:
    QString trackPath_ = QStringLiteral( "0" );
    QString contains_;
    QString absent_;
};

/**
 * `track-detail-section` — collapse or expand ONE section of the Track Detail
 * dock by clicking its REAL header button, on a panel built for `trackPath`.
 * `section` is plugins | feelflow | sliders; `collapsed` is ABSOLUTE (the
 * `collapse-track` rule), so the header is clicked only when the panel does
 * not already show the wanted state. REJECTED for an unknown section or a
 * path that names no track. Not undoable: the state is a project PROPERTY,
 * view state that does not dirty the project (sprojectprops.h).
 */
class STrackDetailSectionAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "track-detail-section" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
private:
    QString trackPath_ = QStringLiteral( "0" );
    QString section_   = QStringLiteral( "feelflow" );
    bool    collapsed_ = true;
};

/**
 * `assert-track-detail-sections` — `contains` / `absent` against
 * STrackDetailPanel::describeSections() on a FRESH panel for `trackPath`:
 *   plugins=<0|1>,feelflow=<0|1>,sliders=<0|1>|pluginFontPt=<n>|pluginTreeFont=<0|1>
 * (1 = collapsed). A fresh panel is the point: the state must come from the
 * PROJECT, never from the panel instance that was clicked.
 */
class SAssertTrackDetailSectionsAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-track-detail-sections" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
private:
    QString trackPath_ = QStringLiteral( "0" );
    QString contains_;
    QString absent_;
};

#endif // _SUIFONTTESTACTIONS_H_
