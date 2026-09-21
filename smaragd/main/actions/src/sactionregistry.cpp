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
    // SORTED, and that is load-bearing rather than cosmetic. factories_ is a
    // QHash, and Qt 6 randomises its hash seed per process, so keys() comes back
    // in a DIFFERENT ORDER ON EVERY RUN. action_roundtrip_test walks this list to
    // audit all 263 verbs, which meant it constructed and destroyed them in a
    // fresh order each time: a latent fault in one verb would surface as an
    // unreproducible "N in 30" crash, and a failure report could not be replayed
    // because the order that produced it was gone with the process (QBX-103).
    //
    // Sorting costs one pass over a few hundred short strings, once, and makes
    // the audit replayable. Callers that want the hash order have none -- the
    // only two callers are that test and main.cpp's --list-actions dump, and
    // both are better off alphabetical.
    QStringList names = factories_.keys();
    names.sort();
    return names;
}
