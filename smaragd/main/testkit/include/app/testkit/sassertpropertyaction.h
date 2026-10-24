#ifndef SASSERTPROPERTYACTION_H
#define SASSERTPROPERTYACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: a project PROPERTY has the expected value - the read
 * side of `set-property` (QBX-212).
 *
 * WHY IT EXISTS. Several consumers read a property and never a view: the
 * audio and MIDI recorders, the MIDI out pump and the cycle all take the
 * range from `rangeStart` / `rangeEnd` / `rangeValid`, so a view that shows
 * the right range while the property says another is a defect no view-level
 * assertion can see.
 *
 * XML format:
 * <assert-property key="rangeStart" equals="725035"/>
 *
 * Parameters:
 * - key:    the property key (the spellings are in SProjectProps)
 * - equals: the expected value as QVariant::toString() spells it - "true",
 *           "725035", "bars". A property that is not set reads as "".
 */
class SAssertPropertyAction : public SAction {
public:
    SAssertPropertyAction() = default;

    QString name() const override { return QStringLiteral( "assert-property" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString key_;
    QString equals_;
};

#endif // SASSERTPROPERTYACTION_H
