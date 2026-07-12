#ifndef _SDETAILEDITORS_H_
#define _SDETAILEDITORS_H_

class QWidget;
class SObject;

/**
 * Detail-editor factory (proposal 14, Phase 6): model objects must not
 * construct their view widgets (that coupled objects/* to timeline/pluginui).
 * A UI module registers a factory for an object class name from a static
 * initializer; SObject::getDetailEditWidget implementations look it up.
 * Same OBJECT-library no-TU-elision constraint as every other registry.
 */
namespace sdetaileditors {

typedef QWidget *(*Factory)( SObject &obj, QWidget *parent );

// className is the Qt meta-object class name (e.g. "SStdMixer").
void registerEditor( const char *className, Factory factory );

// Returns null when no editor is registered for obj's class.
QWidget *create( SObject &obj, QWidget *parent );

}  // namespace sdetaileditors

#endif
