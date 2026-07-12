#include <QtTest/QtTest>
#include <QDomDocument>
#include "app/selection/ssetselectionaction.h"
#include "app/selection/saddtoselectionaction.h"
#include "app/selection/sremovefromselectionaction.h"
#include "app/selection/sclearselectionaction.h"
#include "app/selection/stoggleselectionaction.h"
#include "app/selection/sselectionmanager.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"

class TestSelectionActions : public QObject
{
    Q_OBJECT

private slots:
    void testSetSelectionAction();
    void testAddToSelectionAction();
    void testRemoveFromSelectionAction();
    void testClearSelectionAction();
    void testToggleSelectionAction();
    void testXmlSerialization();
    void testInverseChaining();
};

void TestSelectionActions::testSetSelectionAction()
{
    // Create action with empty paths (clear selection)
    SSetSelectionAction action;
    QCOMPARE(action.name(), QStringLiteral("set-selection"));

    // Create action with paths
    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    paths << (QList<int>() << 1 << 2);
    SSetSelectionAction setAction(paths);

    // Apply should return true with valid inverse
    SProject *proj = SAppContext::get().getCurrentProject();
    SApplyResult result = setAction.apply(proj);
    QVERIFY(result.success);
    QVERIFY(result.inverse != nullptr);
    QCOMPARE(result.inverse->name(), QStringLiteral("set-selection"));

    delete result.inverse;
}

void TestSelectionActions::testAddToSelectionAction()
{
    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    SAddToSelectionAction addAction(paths);
    QCOMPARE(addAction.name(), QStringLiteral("add-to-selection"));

    SProject *proj = SAppContext::get().getCurrentProject();
    SApplyResult result = addAction.apply(proj);
    QVERIFY(result.success);
    QVERIFY(result.inverse != nullptr);
    // Inverse of add-to-selection should be remove-from-selection
    QCOMPARE(result.inverse->name(), QStringLiteral("remove-from-selection"));

    delete result.inverse;
}

void TestSelectionActions::testRemoveFromSelectionAction()
{
    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    SRemoveFromSelectionAction removeAction(paths);
    QCOMPARE(removeAction.name(), QStringLiteral("remove-from-selection"));

    SProject *proj = SAppContext::get().getCurrentProject();
    SApplyResult result = removeAction.apply(proj);
    QVERIFY(result.success);
    QVERIFY(result.inverse != nullptr);
    // Inverse of remove-from-selection should be add-to-selection
    QCOMPARE(result.inverse->name(), QStringLiteral("add-to-selection"));

    delete result.inverse;
}

void TestSelectionActions::testClearSelectionAction()
{
    SClearSelectionAction clearAction;
    QCOMPARE(clearAction.name(), QStringLiteral("clear-selection"));

    SProject *proj = SAppContext::get().getCurrentProject();
    SApplyResult result = clearAction.apply(proj);
    QVERIFY(result.success);
    QVERIFY(result.inverse != nullptr);
    // Inverse of clear-selection should be set-selection with prior paths
    QCOMPARE(result.inverse->name(), QStringLiteral("set-selection"));

    delete result.inverse;
}

void TestSelectionActions::testToggleSelectionAction()
{
    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    SToggleSelectionAction toggleAction(paths);
    QCOMPARE(toggleAction.name(), QStringLiteral("toggle-selection"));

    SProject *proj = SAppContext::get().getCurrentProject();
    SApplyResult result = toggleAction.apply(proj);
    QVERIFY(result.success);
    QVERIFY(result.inverse != nullptr);
    // Inverse of toggle-selection should be toggle-selection (self-inverse)
    QCOMPARE(result.inverse->name(), QStringLiteral("toggle-selection"));

    delete result.inverse;
}

void TestSelectionActions::testXmlSerialization()
{
    // Create action with paths
    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    paths << (QList<int>() << 1 << 2 << 3);
    SSetSelectionAction originalAction(paths);

    // Serialize to XML
    QDomDocument doc;
    QDomElement elem = doc.createElement("action");
    originalAction.writeXml(elem);

    // Check XML attributes
    QVERIFY(elem.hasAttribute("paths"));
    QString pathsStr = elem.attribute("paths");
    QVERIFY(pathsStr.contains("|"));
    QVERIFY(pathsStr.contains(","));

    // Deserialize
    SSetSelectionAction deserializedAction;
    QVERIFY(deserializedAction.readXml(elem, 1));
    QCOMPARE(deserializedAction.name(), originalAction.name());

    // Serialize again and compare
    QDomElement elem2 = doc.createElement("action");
    deserializedAction.writeXml(elem2);
    QCOMPARE(elem2.attribute("paths"), elem.attribute("paths"));
}

void TestSelectionActions::testInverseChaining()
{
    // Test undo/redo chain: SetSelection -> Toggle -> Clear -> Undo all

    QList<QList<int>> paths;
    paths << (QList<int>() << 0 << 1);
    paths << (QList<int>() << 1 << 2);

    SProject *proj = SAppContext::get().getCurrentProject();

    // Apply SetSelection
    SSetSelectionAction setAction(paths);
    SApplyResult result1 = setAction.apply(proj);
    QVERIFY(result1.success);
    SAction *inverse1 = result1.inverse;

    // Apply ToggleSelection (should toggle the same paths)
    SToggleSelectionAction toggleAction(paths);
    SApplyResult result2 = toggleAction.apply(proj);
    QVERIFY(result2.success);
    SAction *inverse2 = result2.inverse;

    // Apply ClearSelection
    SClearSelectionAction clearAction;
    SApplyResult result3 = clearAction.apply(proj);
    QVERIFY(result3.success);
    SAction *inverse3 = result3.inverse;

    // Undo in reverse order (should be inverses of the operations)
    QVERIFY(inverse3 != nullptr);
    QVERIFY(inverse2 != nullptr);
    QVERIFY(inverse1 != nullptr);

    delete inverse3;
    delete inverse2;
    delete inverse1;
}

QTEST_MAIN(TestSelectionActions)
#include "test_selection_actions.moc"
