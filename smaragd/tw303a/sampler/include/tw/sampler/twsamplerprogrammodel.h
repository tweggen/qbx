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

class twSamplerLibrary;

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

    // --- the pads (nassau-digital DESIGN §7, §13 "Settled on 2026-11-03, pads") --
    // A pad names a KEY; its zone is the zone covering that key. Pad n is the
    // index over all banks (n = 16·bank + pad, counting from 0) and defaults to
    // key 36 + n; the program's <Pads> table holds the pads off their default.
    // All of it is the core's (nd::Program::padKey, padsOnKey, ...).
    static constexpr int kNumPads = 96;   // six banks of 16 (the author, 2026-11-03)
    // Pad n's key: its table row's, else 36 + n, else -1 (n 92..95 with no row,
    // or n out of 0..95).
    int padKey( int n ) const;
    // The table as rows (n, key) in the order the core writes them.
    std::vector<std::pair<int, int>> padRows() const;
    // The pads playing `key`, lowest first.
    std::vector<int> padsOnKey( int key ) const;
    // The pad a device note on `key` selects: `currentPad` if it plays the
    // key, else the lowest-numbered pad that does; -1 when none does.
    int padSelectedByKey( int key, int currentPad ) const;
    // The zones covering `key` (keyLo <= key <= keyHi, any velocity), in
    // program order.
    std::vector<std::uint32_t> zonesCoveringKey( int key ) const;

    static twSamplerParamKind       paramKind( twSamplerScope scope, const std::string &attr );
    static std::vector<std::string> paramNames( twSamplerScope scope );
    // A NUMERIC parameter's range as the core's table bounds it (QBX-279: the
    // editor's knobs span it). False for an unknown or non-numeric parameter.
    static bool paramRange( twSamplerScope scope, const std::string &attr, double *lo,
                            double *hi );
    // True when the core's table types the numeric parameter as an integer.
    static bool paramIsInteger( twSamplerScope scope, const std::string &attr );

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
    // with fresh ids from nextId. The ZONE may name an explicit `id` (QBX-265):
    // accepted when it is free and was never handed out (>= nextId), and nextId
    // then moves past it; REFUSED when it is in use or below nextId. A LAYER's
    // `id` is always refused (layers take the ids after the zone's), and so is
    // any attribute the table does not know. Returns the zone's id, 0 on a
    // refusal.
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

    // Point pad n at `key` (the core's nd::setPadKey): only the key changes,
    // the default key removes the row. `priorKey` receives the key before the
    // edit, which undoes it (-1 for a pad that had none, which is a valid
    // `key` for exactly such a pad). Refused (false, `error`): n out of 0..95,
    // key out of 0..127.
    bool setPadKey( int n, int key, int *priorKey, std::string *error );

    struct Impl;

private:
    friend class twSamplerLibrary;   // applied() fills a model from the core's program
    std::unique_ptr<Impl> impl_;
};

// twSamplerLibrary — a LIBRARY program (nassau-digital DESIGN §8): a read-only
// BASE program with the user's OVERLAY of edits on top, a thin forwarder to the
// core's nd_overlay (QBX-278). What plays is applied(): the base with the
// overlay applied, edit by edit. The base is never written; the overlay's
// `base` attribute says where it came from (qbx's library path, carried and
// written back). Qt-free, main thread only, never on the audio thread.
//
// The PAD REASSIGNMENT (`set-pad`, the author's decision of 2026-11-04):
// recordPad() appends one set-pad with key.was (the pad's key before: its
// row's, its default 36 + n, or "none"); hiding a base pad row (`hide-pad`,
// the same day): hidePad(). Since QBX-282 every other program edit too:
// recordSet(), recordAddZone(), recordAddLayer(), recordHideZone(),
// recordHideLayer(), and flattened() (below).
// rebase() accepts the current base (the core's rebaseOverlay): every drifted
// edit's `.was` becomes what the base has under it, so the report clears and
// nothing that plays changes.
class twSamplerLibrary {
public:
    twSamplerLibrary();
    twSamplerLibrary( const twSamplerLibrary &o );
    twSamplerLibrary &operator=( const twSamplerLibrary &o );
    ~twSamplerLibrary();

    // Both texts through the core's readers; an empty overlay text is an
    // empty overlay. False (and `error`) when either is refused.
    static bool fromXml( const std::string &baseXml, const std::string &overlayXml,
                         twSamplerLibrary &out, std::string *error = nullptr );
    std::string baseXml() const;      // the core's writer
    std::string overlayXml() const;   // byte-stable (the core keeps every verb as written)

    // Replace the base (a newer version of the library), keeping the overlay.
    bool setBase( const std::string &baseXml, std::string *error );
    void setBasePath( const std::string &path );   // the overlay's `base` attribute
    std::string basePath() const;
    std::string libraryVersion() const;            // the base's, "" when it has none
    std::string baseVersion() const;               // the overlay's
    std::size_t editCount() const;

    // The base with the overlay applied.
    twSamplerProgramModel applied() const;

    // THE REPORT (DESIGN §8): one line per edit that is not plainly applied,
    // in order, plus the library-version mismatch first, joined by "; ":
    //   "libraryVersion 3->4; #0 set-pad drifted key 36->42; #2 set-zone orphaned 17"
    // "" when every edit applied as written and the versions agree.
    std::string report() const;
    int drifted() const;
    int orphaned() const;
    bool versionMismatch() const;

    // Reassign pad n to `key` as an overlay edit (core recordPadEdit). False
    // (and `error`) when the core refuses: n outside 0..95, key outside 0..127.
    bool recordPad( int n, int key, int *priorKey, std::string *error );
    // Hide pad n's BASE row from what plays as an overlay edit (core
    // recordHidePad, the author's decision of 2026-11-04): the pad goes back
    // to its default key, or a pad 92..95 ceases to exist. False (and
    // `error`) when the core refuses: n outside 0..95, no base row, or the
    // row does not play now.
    bool hidePad( int n, std::string *error );
    // Accept the base: re-stamp baseVersion, refresh the drifted `.was`.
    // Returns how many edits were refreshed.
    int rebase();

    // --- EVERY PROGRAM EDIT AS AN OVERLAY EDIT (QBX-282; DESIGN §8, the
    // author's answers of 2026-11-07). Each runs the core's record*() on what
    // plays now (the base with the overlay applied), so a refusal is the
    // core's, all or nothing (false, `error`, the overlay unchanged).
    //
    // Parameters: one set-zone per zone (a list or ALL expanded over what plays
    // now), one set-layer per layer, or one set-program; a relative edit is
    // stored as its absolute result, each parameter with its `.was`.
    bool recordSet( twSamplerScope scope, bool all, const std::vector<std::uint32_t> &ids,
                    const std::vector<twSamplerEdit> &edits, std::string *error );
    // The id the overlay's next add hands out: the core's own choice
    // (nd::recordAddZone on copies), never a second rule. What a host that
    // must name an added zone or layer in the same step predicts.
    std::uint32_t nextOverlayId() const;
    // The overlay's stored `nextId` (0 until it has added something), and
    // raising it: undoing an add keeps it advanced, so ids are never reused.
    std::uint32_t overlayNextId() const;
    void raiseOverlayNextId( std::uint32_t atLeast );
    // A zone (and its layers) from attribute text, as twSamplerProgramModel::
    // addZone takes it, recorded as add-zone + one add-layer per layer with
    // ids from the overlay range. An explicit `id` is accepted only when it is
    // nextOverlayId(). Returns the zone's id, 0 on a refusal.
    std::uint32_t recordAddZone( const twSamplerAttrs &zone, const std::vector<twSamplerAttrs> &layers,
                                 std::string *error );
    // A layer into any zone that plays (the base's or the overlay's). Its id, 0 on a refusal.
    std::uint32_t recordAddLayer( std::uint32_t zoneId, const twSamplerAttrs &layer, std::string *error );
    // Removing a zone or a layer that plays, a base one or one the overlay
    // added: a stored hide-zone / hide-layer.
    bool recordHideZone( std::uint32_t id, std::string *error );
    bool recordHideLayer( std::uint32_t id, std::string *error );
    // Every `src` an overlay verb writes, in order, orphaned verbs included:
    // the asset links the overlay names (a host keeps them held).
    std::vector<std::string> overlaySrcs() const;
    // FLATTEN (the core's flatten()): a program of its own that plays what the
    // library plays, the overlay's ids kept, no libraryVersion. `dropped`
    // names the edits that did not apply and so are not in it
    // ("#2 set-zone orphaned; #5 add-zone refused"), "" when none.
    twSamplerProgramModel flattened( std::string *dropped ) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace audio

#endif
