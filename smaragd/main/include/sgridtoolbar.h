#ifndef _SGRIDTOOLBAR_H
#define _SGRIDTOOLBAR_H

#include <QToolBar>
#include <QGridLayout>
#include <QList>

class QAction;

/**
 * Grid-based toolbar widget that arranges actions in a compact grid layout.
 * Similar to Reaper's compact icon grid in the upper left.
 *
 * Features:
 * - Arranges toggle buttons in rows and columns
 * - Compact, fixed-size button grid
 * - Configurable columns and button size
 * - Wraps to next row when full
 */
class SGridToolbar : public QToolBar
{
    Q_OBJECT
public:
    explicit SGridToolbar(const QString &title, QWidget *parent = nullptr);

    /**
     * Add an action to the grid toolbar.
     * Automatically arranges in grid layout.
     */
    void addGridAction(QAction *action);

    /**
     * Set the number of columns in the grid.
     * Default is 7 (like Reaper).
     */
    void setColumns(int cols);

    /**
     * Set the button size (width and height are equal).
     * Default is 26x26 pixels.
     */
    void setButtonSize(int size);

private:
    int columns_;
    int buttonSize_;
    int currentRow_;
    int currentCol_;
    QGridLayout *gridLayout_;
    QList<QAction*> actions_;
};

#endif
