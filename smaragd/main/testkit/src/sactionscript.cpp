#include "app/testkit/sactionscript.h"
#include "app/actions/saction.h"
#include "app/actions/sactionregistry.h"
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

SActionScript::SActionScript()
{
}

SActionScript::~SActionScript()
{
    qDeleteAll(actions_);
}

bool SActionScript::readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = QString("Cannot open file: %1").arg(path);
        return false;
    }

    // Record the script's directory so relative sample paths resolve next to
    // the .qxa regardless of the process working directory.
    baseDir_ = QFileInfo(path).absolutePath();

    QString errorMsg;
    QDomDocument doc;
    if (!doc.setContent(&file, &errorMsg)) {
        file.close();
        error_ = QString("XML parse error: %1").arg(errorMsg);
        return false;
    }
    file.close();

    return readXml(doc);
}

bool SActionScript::readXml(const QDomDocument &doc)
{
    QDomElement root = doc.documentElement();
    if (root.tagName() != "SActionScript") {
        error_ = "Root element must be <SActionScript>";
        return false;
    }

    // Parse setup element (optional).
    QDomElement setupEl = root.firstChildElement("setup");
    if (!setupEl.isNull()) {
        QString projectKind = setupEl.attribute("project", "new");
        if (projectKind == "new") {
            setup_.kind = Setup::New;
            setup_.file = QString();
        } else if (projectKind == "load") {
            setup_.kind = Setup::Load;
            setup_.file = setupEl.attribute("file", "");
        } else {
            error_ = QString("Unknown setup project kind: %1").arg(projectKind);
            return false;
        }
    }

    // Parse actions list.
    QDomElement actionsEl = root.firstChildElement("actions");
    if (!actionsEl.isNull()) {
        for (QDomElement child = actionsEl.firstChildElement();
             !child.isNull();
             child = child.nextSiblingElement()) {

            SAction *action = SActionRegistry::instance().createFromXml(child);
            if (!action) {
                error_ = QString("Unknown or malformed action: %1").arg(child.tagName());
                return false;
            }

            // Phase 4: Track expectReject metadata for negative tests
            ActionMeta meta;
            meta.action = action;
            meta.expectReject = child.attribute("expectReject", "false") == "true";

            actions_.append(action);
            actionsMeta_.append(meta);
        }
    }

    // Parse assertions (optional).
    QDomElement assertionsEl = root.firstChildElement("assertions");
    if (!assertionsEl.isNull()) {
        for (QDomElement child = assertionsEl.firstChildElement();
             !child.isNull();
             child = child.nextSiblingElement()) {

            Assertion assertion;
            assertion.kind = child.tagName();

            // Collect all attributes as arguments.
            QDomNamedNodeMap attrs = child.attributes();
            for (int i = 0; i < attrs.length(); ++i) {
                QDomAttr attr = attrs.item(i).toAttr();
                assertion.args[attr.name()] = attr.value();
            }

            assertions_.append(assertion);
        }
    }

    // Check for verify-undo element.
    QDomElement verifyEl = root.firstChildElement("verify-undo");
    verifyUndo_ = !verifyEl.isNull();

    return true;
}

QDomDocument SActionScript::toXml() const
{
    QDomDocument doc;
    doc.appendChild(doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\""));

    QDomElement rootEl = doc.createElement("SActionScript");
    rootEl.setAttribute("version", "1");
    rootEl.setAttribute("name", "action_script");
    doc.appendChild(rootEl);

    // Write setup element.
    QDomElement setupEl = doc.createElement("setup");
    setupEl.setAttribute("project", setup_.kind == Setup::New ? "new" : "load");
    if (setup_.kind == Setup::Load) {
        setupEl.setAttribute("file", setup_.file);
    }
    rootEl.appendChild(setupEl);

    // Write actions.
    QDomElement actionsEl = doc.createElement("actions");
    for (const SAction *action : actions_) {
        QDomElement actionEl = doc.createElement(action->name());
        if (action->formatVersion() != 1) {
            actionEl.setAttribute("version", action->formatVersion());
        }
        action->writeXml(actionEl);
        actionsEl.appendChild(actionEl);
    }
    rootEl.appendChild(actionsEl);

    // Write assertions.
    QDomElement assertionsEl = doc.createElement("assertions");
    for (const Assertion &assertion : assertions_) {
        QDomElement assertEl = doc.createElement(assertion.kind);
        for (auto it = assertion.args.begin(); it != assertion.args.end(); ++it) {
            assertEl.setAttribute(it.key(), it.value());
        }
        assertionsEl.appendChild(assertEl);
    }
    rootEl.appendChild(assertionsEl);

    // Write verify-undo element.
    if (verifyUndo_) {
        QDomElement verifyEl = doc.createElement("verify-undo");
        rootEl.appendChild(verifyEl);
    }

    return doc;
}

bool SActionScript::writeFile(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error_ = QString("Cannot write file: %1").arg(path);
        return false;
    }

    QDomDocument doc = toXml();
    file.write(doc.toByteArray(2));  // 2-space indent
    file.close();

    return true;
}
