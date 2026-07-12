#ifndef SACTIONREGISTRY_H
#define SACTIONREGISTRY_H

#include <QString>
#include <QHash>
#include <functional>

class SAction;
class QDomElement;

// Singleton registry: maps action name → factory function
// Used for deserializing actions from XML and for scripting introspection.
class SActionRegistry {
public:
    using Factory = std::function<SAction*()>;

    static SActionRegistry &instance();

    // Register an action type under a name.
    void registerType(const QString &name, Factory f);

    // Create a new instance of the named action type.
    SAction *create(const QString &name) const;

    // Deserialize an action from XML: reads name + formatVersion attributes.
    SAction *createFromXml(const QDomElement &elem) const;

    // List all registered action names (for scripting).
    QStringList knownNames() const;

private:
    SActionRegistry() = default;
    QHash<QString, Factory> factories_;
};

#endif // SACTIONREGISTRY_H
