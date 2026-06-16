#include "sgridtoolbar.h"
#include <QAction>
#include <QToolButton>
#include <QWidget>

SGridToolbar::SGridToolbar(const QString &title, QWidget *parent)
    : QToolBar(title, parent),
      columns_(7),
      buttonSize_(26),
      currentRow_(0),
      currentCol_(0)
{
    // Create a container widget with grid layout
    QWidget *container = new QWidget(this);
    gridLayout_ = new QGridLayout(container);

    // Tight spacing for compact grid
    gridLayout_->setSpacing(2);
    gridLayout_->setContentsMargins(2, 2, 2, 2);
    gridLayout_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    // Add the container to the toolbar
    addWidget(container);

    // Set fixed height based on button size (room for 2 rows by default)
    setIconSize(QSize(buttonSize_, buttonSize_));
    setFixedHeight(2 * buttonSize_ + 10);  // 2 rows + spacing
}

void SGridToolbar::addGridAction(QAction *action)
{
    if (!action) return;

    actions_.append(action);

    // Create a tool button for the action
    QToolButton *btn = new QToolButton(this);
    btn->setDefaultAction(action);
    btn->setIconSize(QSize(buttonSize_, buttonSize_));
    btn->setFixedSize(buttonSize_ + 4, buttonSize_ + 4);

    // Add to grid at current position
    gridLayout_->addWidget(btn, currentRow_, currentCol_);

    // Move to next position
    ++currentCol_;
    if (currentCol_ >= columns_) {
        currentCol_ = 0;
        ++currentRow_;
        // Expand toolbar height if needed
        setFixedHeight((currentRow_ + 1) * buttonSize_ + 10);
    }
}

void SGridToolbar::setColumns(int cols)
{
    columns_ = (cols > 0) ? cols : 7;
}

void SGridToolbar::setButtonSize(int size)
{
    buttonSize_ = (size > 0) ? size : 26;
    setIconSize(QSize(buttonSize_, buttonSize_));

    // Update all existing buttons
    for (int row = 0; row < gridLayout_->rowCount(); ++row) {
        for (int col = 0; col < gridLayout_->columnCount(); ++col) {
            QLayoutItem *item = gridLayout_->itemAtPosition(row, col);
            if (item && item->widget()) {
                QToolButton *btn = qobject_cast<QToolButton*>(item->widget());
                if (btn) {
                    btn->setIconSize(QSize(buttonSize_, buttonSize_));
                    btn->setFixedSize(buttonSize_ + 4, buttonSize_ + 4);
                }
            }
        }
    }

    // Recalculate height
    setFixedHeight((currentRow_ + 1) * buttonSize_ + 10);
}
