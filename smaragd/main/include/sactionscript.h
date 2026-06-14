#ifndef SACTIONSCRIPT_H
#define SACTIONSCRIPT_H

#include <QString>
#include <QList>
#include <QMap>
#include <QDomDocument>

class SAction;

// Container for an action script: setup metadata, action sequence, assertions.
class SActionScript {
public:
    struct Setup {
        enum Kind { New, Load } kind = New;
        QString file;
    };

    struct Assertion {
        QString kind;  // assert-track-count, assert-project-matches, etc.
        QMap<QString, QString> args;
    };

    SActionScript();
    ~SActionScript();

    // Parse a .qxa file. Returns false + fills error() on malformed input or
    // an unknown action verb (fail fast).
    bool readFile(const QString &path);
    bool readXml(const QDomDocument &doc);

    // Serialize the current action list back out (round-trip / record mode).
    QDomDocument toXml() const;
    bool writeFile(const QString &path) const;

    const Setup &setup() const { return setup_; }
    const QList<SAction*> &actions() const { return actions_; }
    const QList<Assertion> &assertions() const { return assertions_; }
    bool verifyUndo() const { return verifyUndo_; }

    QString error() const { return error_; }

private:
    Setup setup_;
    QList<SAction*> actions_;
    QList<Assertion> assertions_;
    bool verifyUndo_ = false;
    mutable QString error_;
};

#endif // SACTIONSCRIPT_H
