#ifndef SCOLLAPSIBLESECTION_H
#define SCOLLAPSIBLESECTION_H

#include <QString>
#include <QWidget>

class QToolButton;
class QVBoxLayout;

/**
 * A titled section with a disclosure triangle (QBX-102): a flat header button
 * showing a right-pointing arrow when collapsed and a down-pointing one when
 * expanded, above a content area that is shown or hidden with it.
 *
 * THE CONTENT IS NOT INDENTED. The content layout has zero margins, so a
 * widget moved into a section occupies exactly the width it had before; the
 * section adds a header row and nothing else.
 *
 * THE SECTION HOLDS NO STATE OF ITS OWN BEYOND WHAT IT SHOWS. Whoever owns it
 * decides where the collapsed flag lives (the Track Detail dock keeps it in
 * SProject properties) and answers `toggleRequested` by calling
 * `setExpanded`. That is what lets a state shared by many panels be applied
 * from one place instead of each section deciding on its own.
 */
class SCollapsibleSection : public QWidget {
    Q_OBJECT
public:
    /// `id` is a stable token ("plugins", "feelflow", "sliders") used for the
    /// header's objectName and in `describe()`-style reports; `title` is what
    /// the header reads.
    SCollapsibleSection( const QString &id, const QString &title,
                         QWidget *parent = nullptr );

    QVBoxLayout *contentLayout() const { return contentLayout_; }
    QToolButton *headerButton() const { return header_; }
    QString id() const { return id_; }

    bool isExpanded() const { return expanded_; }
    void setExpanded( bool expanded );

signals:
    /// The header was clicked. Carries the state the user asked FOR, i.e. the
    /// opposite of the current one; the owner decides and calls setExpanded.
    void toggleRequested( bool wantExpanded );

private:
    QString      id_;
    QToolButton *header_        = nullptr;
    QWidget     *content_       = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;
    bool         expanded_      = true;
};

#endif // SCOLLAPSIBLESECTION_H
