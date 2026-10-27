#ifndef _TWSAMPLERPROGRAMMODEL_H_
#define _TWSAMPLERPROGRAMMODEL_H_

// twsamplerprogrammodel.h — the EDITABLE NassauDigital program (proposal 56
// §4.4, Q1; nassau-digital DESIGN §2, §2.1, §12).
//
// twSamplerProgram (twsamplertypes.h) is the IMMUTABLE snapshot the
// instrument plays. This is the mutable program the app's model object
// (objects/sampler's SSamplerProgram) owns and edits: one core nd::Program,
// behind a pimpl, because the core is linked PRIVATE into tw_sampler
// (proposal 56 invariant 1) and no other module may name a core type.
//
// IT IS A THIN FORWARDER, ON PURPOSE. Every rule a program obeys is the core's:
// the parameter tables (names, kinds, ranges, defaults), the ALL scope, the
// relative-edit clamp and its refusal on an enum, "every edit returns the
// absolute values it replaced", ids handed out from nextId and never reused,
// and the one XML grammar with its unknown-attribute preservation. Nothing here
// re-states any of them; qbx never parses a program attribute itself
// (invariant 8). What this file adds is only what a host needs and the core's
// own API does not spell out:
//
//   * add from ATTRIBUTE TEXT (an action's XML), refusing an attribute the
//     table does not know — the core's addZone() takes a built Zone, and a
//     typo in a script must be refused, not kept as an "unknown attribute";
//   * a removed zone / layer as the CORE'S OWN XML text plus where it was, so
//     an undo entry can carry it, and its reinsertion VALIDATED (ids free and
//     already handed out) — the core's reinsert checks less than that;
//   * read access by name for assertions and describers.
//
// Qt-free (THREADING.md rule 1). Main thread / worker only: everything here
// allocates. Never the audio thread — that is what snapshot() is for.

#include "tw/sampler/twsamplertypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace audio {

// Which object a parameter belongs to.
enum class twSamplerScope { Program, Zone, Layer };

// The kind of a parameter, as the core's table declares it. Only Numeric may
// take a RELATIVE edit (DESIGN §2.1); the core refuses one on anything else.
enum class twSamplerParamKind { Unknown, Numeric, Enum, Bool, String };

// One parameter edit: ABSOLUTE (`value`, as the XML text the core parses) or
// RELATIVE (`delta`, added to each target's own value, then clamped per target).
struct twSamplerEdit {
    std::string attr;
    bool        relative = false;
    double      delta    = 0.0;
    std::string value;
};

// One replaced value: the target's id (0 = the program itself), the parameter
// and its PREVIOUS ABSOLUTE value as XML text — what an undo writes back.
struct twSamplerPrior {
    std::uint32_t id = 0;
    std::string   attr;
    std::string   value;
    bool operator==( const twSamplerPrior &o ) const
    { return id == o.id && attr == o.attr && value == o.value; }
};

// Attribute name -> text, in the order given.
using twSamplerAttrs = std::vector<std::pair<std::string, std::string>>;

class twSamplerProgramModel {
public:
    twSamplerProgramModel();                       // the core's default (empty) program
    twSamplerProgramModel( const twSamplerProgramModel &o );
    twSamplerProgramModel &operator=( const twSamplerProgramModel &o );
    ~twSamplerProgramModel();

    // --- the one grammar ----------------------------------------------------
    // Parse the <NassauDigitalProgram> element as text. On a refusal `out` is
    // left unchanged and the core's reason is in `error`.
    static bool fromXml( const std::string &xml, twSamplerProgramModel &out,
                         std::string *error = nullptr );
    // Written by the core: every known attribute in table order, then the
    // unknown ones it was read with (no XML declaration).
    std::string toXml() const;

    // STRUCTURAL EQUALITY, the core's own (nd::Program::operator==): every
    // parameter, every zone and layer in order with its id, nextId, and the
    // unknown attributes and elements in order.
    bool operator==( const twSamplerProgramModel &o ) const;
    bool operator!=( const twSamplerProgramModel &o ) const { return !( *this == o ); }
    // The same, except nextId. An undone add-zone leaves nextId where the add
    // put it (DESIGN §2: ids are never reused, GATES C1.3, proposal 56 Q1.q2),
    // so this is the comparison "undone exactly" means for it — and the
    // assertion that uses it checks nextId separately.
    bool equalsIgnoringNextId( const twSamplerProgramModel &o ) const;

    // An immutable snapshot for the instrument (a COPY: editing this model
    // afterwards never reaches it). Allocates; never the audio thread.
    std::shared_ptr<const twSamplerProgram> snapshot() const;

    // --- reading --------------------------------------------------------------
    std::uint32_t              nextId() const;
    std::size_t                zoneCount() const;
    std::vector<std::uint32_t> zoneIds() const;          // in program order
    std::vector<std::uint32_t> layerIds( std::uint32_t zoneId ) const;
    bool hasZone( std::uint32_t id ) const;
    bool hasLayer( std::uint32_t id ) const;
    // A parameter's value as the core writes it. `id` is ignored for Program.
    // False for an unknown parameter or a missing object.
    bool value( twSamplerScope scope, std::uint32_t id, const std::string &attr,
                std::string *out ) const;
    // The zone (with its layers) or the layer as the core writes it, and where
    // it sits (its index in the program / in its zone).
    bool zoneXml( std::uint32_t id, std::string *xml, std::size_t *index ) const;
    bool layerXml( std::uint32_t id, std::string *xml, std::uint32_t *zoneId,
                   std::size_t *index ) const;

    static twSamplerParamKind       paramKind( twSamplerScope scope, const std::string &attr );
    static std::vector<std::string> paramNames( twSamplerScope scope );

    // --- editing (all or nothing: a refusal changes nothing) -------------------
    // Set parameters on the program, on the listed zones / layers, or on every
    // zone (`all`, Zone only). `prior` receives what was replaced.
    bool set( twSamplerScope scope, bool all, const std::vector<std::uint32_t> &ids,
              const std::vector<twSamplerEdit> &edits, std::vector<twSamplerPrior> *prior,
              std::string *error );
    // Write back what a set() returned (an undo). `replaced` receives the values
    // it overwrote, i.e. what redoes it.
    bool restore( twSamplerScope scope, const std::vector<twSamplerPrior> &prior,
                  std::vector<twSamplerPrior> *replaced, std::string *error );

    // Add a zone (and up to four layers) built from attribute text, at the end,
    // with fresh ids from nextId. An `id` attribute is REFUSED (ids are assigned,
    // never taken), and so is any attribute the table does not know. Returns
    // the zone's id, 0 on a refusal.
    std::uint32_t addZone( const twSamplerAttrs &zone, const std::vector<twSamplerAttrs> &layers,
                           std::string *error );
    // Add a layer to a zone, the same way. 0 on a refusal (missing or full zone).
    std::uint32_t addLayer( std::uint32_t zoneId, const twSamplerAttrs &layer, std::string *error );

    // Remove, handing back the object as the core's XML and where it was.
    bool removeZone( std::uint32_t id, std::string *xml, std::size_t *index, std::string *error );
    bool removeLayer( std::uint32_t id, std::string *xml, std::uint32_t *zoneId,
                      std::size_t *index, std::string *error );
    // Put a removed object back with its ids. Refused when the XML does not
    // parse, an id is in use, or an id was never handed out (>= nextId).
    bool reinsertZone( const std::string &xml, std::size_t index, std::string *error );
    bool reinsertLayer( const std::string &xml, std::uint32_t zoneId, std::size_t index,
                        std::string *error );

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace audio

#endif
