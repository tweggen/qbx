#include "app/actions/saction.h"
#include "app/actions/sactionregistry.h"
#include "app/testkit/sactionscript.h"
#include <QDomDocument>
#include <QString>
#include <QStringList>
#include <iostream>

// Test that each action can round-trip through XML:
// create → writeXml → readXml → writeXml, verify identical
bool testActionRoundTrip(const QString &actionName, QString &error)
{
    SActionRegistry &registry = SActionRegistry::instance();

    // Create a default instance.
    SAction *action1 = registry.create(actionName);
    if (!action1) {
        error = QString("Failed to create action: %1").arg(actionName);
        return false;
    }

    // Serialize to XML (action 1).
    QDomDocument doc1;
    QDomElement elem1 = doc1.createElement(action1->name());
    if (action1->formatVersion() != 1) {
        elem1.setAttribute("version", action1->formatVersion());
    }
    action1->writeXml(elem1);
    doc1.appendChild(elem1);

    // Deserialize to a new action (action 2).
    SAction *action2 = registry.createFromXml(elem1);
    if (!action2) {
        error = QString("Failed to deserialize action from XML: %1").arg(actionName);
        delete action1;
        return false;
    }

    // Serialize action 2 to XML.
    QDomDocument doc2;
    QDomElement elem2 = doc2.createElement(action2->name());
    if (action2->formatVersion() != 1) {
        elem2.setAttribute("version", action2->formatVersion());
    }
    action2->writeXml(elem2);
    doc2.appendChild(elem2);

    // Compare the two XML fragments (as text).
    // Convert both to strings for comparison.
    QString xml1 = elem1.toElement().toDocument().toString();
    QString xml2 = elem2.toElement().toDocument().toString();

    // Simpler approach: compare the XML as text after normalizing.
    QString xml1Str = doc1.toString();
    QString xml2Str = doc2.toString();

    delete action1;
    delete action2;

    if (xml1Str != xml2Str) {
        error = QString("Round-trip mismatch for %1:\nFirst:  %2\nSecond: %3")
            .arg(actionName, xml1Str, xml2Str);
        return false;
    }

    return true;
}

// Audit all registered actions for round-trip correctness.
// Exit code: 0 = all pass, 1 = any failure.
int main()
{
    SActionRegistry &registry = SActionRegistry::instance();
    QStringList names = registry.knownNames();

    std::cout << "Testing " << names.size() << " actions for round-trip correctness...\n";

    QStringList failures;

    for (const QString &name : names) {
        QString error;
        if (!testActionRoundTrip(name, error)) {
            failures.append(error);
            std::cout << "FAIL: " << name.toStdString() << "\n";
        } else {
            std::cout << "PASS: " << name.toStdString() << "\n";
        }
    }

    std::cout << "\n";

    if (failures.isEmpty()) {
        std::cout << "All " << names.size() << " actions passed round-trip test.\n";
        return 0;
    } else {
        std::cout << failures.size() << " failures:\n\n";
        for (const QString &failure : failures) {
            std::cout << failure.toStdString() << "\n\n";
        }
        return 1;
    }
}
