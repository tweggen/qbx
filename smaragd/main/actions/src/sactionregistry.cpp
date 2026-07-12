#include "app/actions/sactionregistry.h"
#include "app/actions/saction.h"
#include <QDomElement>

SActionRegistry &SActionRegistry::instance()
{
    static SActionRegistry registry;
    return registry;
}

void SActionRegistry::registerType(const QString &name, Factory f)
{
    factories_[name] = f;
}

SAction *SActionRegistry::create(const QString &name) const
{
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        return nullptr;
    }
    return it.value()();
}

SAction *SActionRegistry::createFromXml(const QDomElement &elem) const
{
    // The tag name is the action name.
    QString name = elem.tagName();
    int version = elem.attribute("version", "1").toInt();

    SAction *action = create(name);
    if (!action) {
        return nullptr;
    }

    if (!action->readXml(elem, version)) {
        delete action;
        return nullptr;
    }

    return action;
}

QStringList SActionRegistry::knownNames() const
{
    return factories_.keys();
}
