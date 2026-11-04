#ifndef SSAMPLERXMLPANE_H
#define SSAMPLERXMLPANE_H

// ssamplerxmlpane.h — the editor's XML command pane (proposal 56 §4.11, A5;
// nassau-digital DESIGN §9.3: "the command field takes the XML itself, with no
// shorthand; it is an editor pane with completion of verbs and attributes";
// QBX-279).
//
// A multi-line editor and an Apply button. Typing runs ONE completion routine
// (completionsAt): right after `<` it offers the instrument's verbs, inside a
// tag after a blank the verb's attributes — its declared ones,
// SAction::knownAttributes() of a registry-made instance, the same list the
// script runner checks a .qxa against — minus `trackPath` and `slotIndex`
// (implied here) and minus those already written. Apply hands the text to
// SSamplerEditor::submitVerbXml: one element, one submit, one undo step.

#include <QString>
#include <QStringList>
#include <QWidget>

class QCompleter;
class QPlainTextEdit;
class QPushButton;
class QStringListModel;
class SSamplerEditor;

class SSamplerXmlPane : public QWidget {
    Q_OBJECT
public:
    explicit SSamplerXmlPane( SSamplerEditor *editor, QWidget *parent = nullptr );

    // The verbs addressed to the instrument, in completion order. The inverse
    // forms (reinsert-*, restore-sampler-values) are left out: they are written
    // by the undo machinery, not by hand.
    static QStringList verbs();

    // What completion offers for `text` with the cursor at `cursor`, and the
    // PREFIX being completed (what an accepted completion replaces). Empty
    // when the cursor is not at a verb or attribute name.
    static QStringList completionsAt( const QString &text, int cursor, QString *prefix );

    // The seams (SSamplerEditor::xmlType / xmlAcceptCompletion / xmlApply).
    void        typeText( const QString &text, int cursor );
    QStringList offered() const;   // what the completer's model holds now
    bool        acceptCompletion( const QString &choice );
    bool        apply();
    QString     text() const;
    QString     status() const { return status_; }

private slots:
    void onTextChanged();
    void insertCompletion( const QString &completion );

private:
    SSamplerEditor   *editor_;
    QPlainTextEdit   *edit_ = nullptr;
    QPushButton      *apply_ = nullptr;
    QCompleter       *completer_ = nullptr;
    QStringListModel *model_ = nullptr;
    QString           prefix_;
    QString           status_;
    bool              inserting_ = false;
};

#endif
