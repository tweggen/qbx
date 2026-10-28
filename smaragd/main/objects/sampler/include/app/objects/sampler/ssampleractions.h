#ifndef _SSAMPLERACTIONS_H_
#define _SSAMPLERACTIONS_H_

// ssampleractions.h — the NassauDigital program verbs (proposal 56 §4.5, Q1;
// nassau-digital DESIGN §2.1, §9.3).
//
// ADDRESSING (A6, B6, Q-5 answered): `trackPath` + `slotIndex`, as the plugin
// verbs, plus a stable zone or layer id:
//
//     <set-zone trackPath="0" slotIndex="0" zone="17" tune="+2"/>
//     <set-zone trackPath="0" slotIndex="0" zone="all" attack="0.01"/>
//
// trackPath may be root-qualified ("Drums:0"); a bare one addresses the
// action's SAction::pathRoot() (empty = the master), the splacements::
// laneBySpec rule. The root lives in the BASE's pathRoot_ and nowhere else
// (actions/CONTRACT.md inv. 6): every class here keeps none of its own.
//
// VALUES are the core's XML text. On a NUMERIC parameter a leading `+` or `-`
// is a RELATIVE edit (DESIGN §2.1), clamped per target by the core; on an enum
// or a bool it is REFUSED by the core, announced, and nothing changes. A
// negative ABSOLUTE value is written with a leading `=` (`pan="=-0.5"`) —
// ASSUMPTION awaiting the author: DESIGN.md makes every signed value relative
// and gives no spelling for a negative absolute one. A text parameter (a
// layer's `src`, the program's `name`) is always taken literally.
//
// UNDO is exact and one step per verb (GATES Q1.1). Every inverse restores
// ABSOLUTE values (C1.5) and is one of the verbs below, so it round-trips
// through XML like any other:
//
//     add-zone        -> remove-zone            (the redo re-inserts the SAME
//     add-layer       -> remove-layer            zone/layer with the same ids;
//                                                a fresh add is never re-run)
//     remove-zone     -> reinsert-zone          (the core's own zone XML, ids
//     remove-layer    -> reinsert-layer          and unknown attributes kept)
//     set-zone, set-layer,
//     set-sampler-program -> restore-sampler-values (the previous absolute
//                                                value of every parameter of
//                                                every target)
//     select-zones    -> select-zones mode="replace" (the previous selection)
//
// Ids are never reused (DESIGN §2): an undone add-zone leaves nextId where the
// add put it, so a new add-zone after it gets a new id (Q1.q2).
//
// EVERY REFUSAL is announced (qWarning, the verb's name first) and mutates
// nothing — the program is edited on a copy (SSamplerProgram::edit) and the
// program object itself is created only by an edit that is going to succeed.

#include "app/actions/saction.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <string>

class SSamplerProgram;

using SSamplerAttrList = QList<QPair<QString, QString>>;

// The address every verb here shares.
class SSamplerAction : public SAction {
public:
    const QString &trackPath() const { return trackPath_; }
    int slotIndex() const { return slotIndex_; }

protected:
    SSamplerAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0 )
        : trackPath_( trackPath ), slotIndex_( slotIndex ) {}

    // The program at this address; `create` makes it on a NassauDigital slot
    // that has none yet. Null after announcing why.
    SSamplerProgram *program( SProject *project, bool create, QString *why ) const;
    // Announce a refusal and return the refused result.
    SApplyResult refuse( const QString &why ) const;

    void writeAddress( QDomElement &elem ) const;
    void readAddress( const QDomElement &elem );
    static QStringList addressAttributes();

    QString trackPath_;
    int     slotIndex_ = 0;
};

// add-zone: zone attributes on the element, up to four <layer .../> children.
class SAddZoneAction : public SSamplerAction {
public:
    SAddZoneAction() = default;
    QString name() const override { return QStringLiteral( "add-zone" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    SSamplerAttrList        zone_;
    QList<SSamplerAttrList> layers_;
    // Filled by the FIRST apply, so a redo puts back the very zone it made
    // (same ids) instead of minting new ones. Not serialized: a property of
    // the moment, like set-plugin-param's previous value.
    std::uint32_t createdId_    = 0;
    std::string   createdXml_;
    std::size_t   createdIndex_ = 0;
};

// remove-zone: `zone`.
class SRemoveZoneAction : public SSamplerAction {
public:
    SRemoveZoneAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0,
                       std::uint32_t zone = 0 )
        : SSamplerAction( trackPath, slotIndex ), zone_( zone ) {}
    QString name() const override { return QStringLiteral( "remove-zone" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    std::uint32_t zone_ = 0;
};

// reinsert-zone: `zone` (its id), `at` (its index), the core's zone XML as
// the element's text. The inverse of remove-zone.
class SReinsertZoneAction : public SSamplerAction {
public:
    SReinsertZoneAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0,
                         std::uint32_t zone = 0, std::size_t at = 0,
                         const std::string &xml = std::string() )
        : SSamplerAction( trackPath, slotIndex ), zone_( zone ), at_( at ), xml_( xml ) {}
    QString name() const override { return QStringLiteral( "reinsert-zone" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    std::uint32_t zone_ = 0;
    std::size_t   at_   = 0;
    std::string   xml_;
};

// add-layer: `zone` + layer attributes.
class SAddLayerAction : public SSamplerAction {
public:
    SAddLayerAction() = default;
    QString name() const override { return QStringLiteral( "add-layer" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString          zone_;
    SSamplerAttrList layer_;
    std::uint32_t    createdId_    = 0;   // as SAddZoneAction
    std::string      createdXml_;
    std::uint32_t    createdZone_  = 0;
    std::size_t      createdIndex_ = 0;
};

// remove-layer: `layer`.
class SRemoveLayerAction : public SSamplerAction {
public:
    SRemoveLayerAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0,
                        std::uint32_t layer = 0 )
        : SSamplerAction( trackPath, slotIndex ), layer_( layer ) {}
    QString name() const override { return QStringLiteral( "remove-layer" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    std::uint32_t layer_ = 0;
};

// reinsert-layer: `layer` (its id), `zone`, `at`, the core's layer XML as text.
class SReinsertLayerAction : public SSamplerAction {
public:
    SReinsertLayerAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0,
                          std::uint32_t layer = 0, std::uint32_t zone = 0, std::size_t at = 0,
                          const std::string &xml = std::string() )
        : SSamplerAction( trackPath, slotIndex ), layer_( layer ), zone_( zone ), at_( at ),
          xml_( xml ) {}
    QString name() const override { return QStringLiteral( "reinsert-layer" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    std::uint32_t layer_ = 0;
    std::uint32_t zone_  = 0;
    std::size_t   at_    = 0;
    std::string   xml_;
};

// set-zone / set-layer / set-sampler-program: ONE class, three spellings; the
// scope is fixed by the registered name (the comp-map verbs' shape).
//   set-zone            `zone` = an id | "all", or `zones` = "3;7;12" | "all"
//   set-layer           `layer` = an id, or `layers` = "18;19"
//   set-sampler-program no selector
class SSetSamplerValuesAction : public SSamplerAction {
public:
    enum class Scope { Program, Zone, Layer };
    explicit SSetSamplerValuesAction( Scope scope = Scope::Zone ) : scope_( scope ) {}
    QString name() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    Scope            scope_;
    QString          selectorAttr_;   // "zone" | "zones" | "layer" | "layers" | ""
    QString          selector_;
    SSamplerAttrList values_;
};

// restore-sampler-values: `scope` = program | zone | layer, and one
// <prior id="…" attr="…" value="…"/> child per replaced value. The inverse of
// the three set verbs, and of itself.
class SRestoreSamplerValuesAction : public SSamplerAction {
public:
    struct Prior {
        std::uint32_t id = 0;
        QString       attr;
        QString       value;
    };
    SRestoreSamplerValuesAction( const QString &trackPath = QStringLiteral( "0" ),
                                 int slotIndex = 0, const QString &scope = QStringLiteral( "zone" ),
                                 const QList<Prior> &prior = {} )
        : SSamplerAction( trackPath, slotIndex ), scope_( scope ), prior_( prior ) {}
    QString name() const override { return QStringLiteral( "restore-sampler-values" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString      scope_;
    QList<Prior> prior_;
};

// select-zones: `zones` = "3;7;12" | "all" | "" (none), `mode` = replace | add
// | toggle. Changes no sound and installs nothing.
class SSelectZonesAction : public SSamplerAction {
public:
    SSelectZonesAction( const QString &trackPath = QStringLiteral( "0" ), int slotIndex = 0,
                        const QString &zones = QString(),
                        const QString &mode = QStringLiteral( "replace" ) )
        : SSamplerAction( trackPath, slotIndex ), zones_( zones ), mode_( mode ) {}
    QString name() const override { return QStringLiteral( "select-zones" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString zones_;
    QString mode_;
};

#endif
