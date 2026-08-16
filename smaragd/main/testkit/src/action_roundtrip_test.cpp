#include "app/actions/saction.h"
#include "app/actions/sactionregistry.h"
#include "app/testkit/sactionscript.h"
#include <QDomDocument>
#include <QString>
#include <QStringList>
#include <cmath>
#include <iostream>

// Audit every registered action for XML round-trip correctness:
//   create → readXml(fixture) → writeXml → readXml → writeXml, verify identical.
//
// Two things changed in proposal 08 M5, because this binary was built but never
// registered with CTest — so it had never gated anything, and it was RED:
//
//  1. FIXTURES. A default-constructed action is not necessarily a VALID one:
//     assert-audio-energy, assert-audio-peak, assert-file-contains and
//     assert-sidecar all (correctly) reject their own default XML, because a
//     filename / path / aspect is mandatory. Testing the default instance
//     therefore tested nothing for them and reported four failures. A verb with
//     a fixture below is loaded from it first, so the round trip runs over
//     REPRESENTATIVE field values instead of zeroes.
//  2. Fixture attributes must SURVIVE. write→read→write cannot catch a field
//     that readXml never reads (it is absent from both sides and compares
//     equal) — which is exactly the shape of the bug M5 fixed in
//     remove-plugin's inverse. So every attribute a fixture declares is also
//     checked against what writeXml produced.

namespace {

struct Fixture {
    const char *verb;
    const char *xml;
};

// One representative element per verb that needs non-default values. Optional
// attributes that are only written when non-default (an `absent='false'` say)
// must NOT appear here — they would legitimately not come back.
const Fixture kFixtures[] = {
    // --- verbs whose readXml validates required attributes -------------------
    { "assert-audio-energy",
      "<assert-audio-energy filename='r.wav' minRms='0.09' maxRms='0.11'"
      " startFrame='48000' frameCount='96000' channel='1'/>" },
    { "assert-audio-peak",
      "<assert-audio-peak filename='r.wav' maxPeak='0.5' startFrame='1024'"
      " frameCount='2048' channel='0'/>" },
    { "assert-audio-length",
      "<assert-audio-length filename='r.wav' minFrames='190000'"
      " maxFrames='200000'/>" },
    // No startFrame: these writers omit their default-valued optional
    // attributes, and a fixture must only declare what comes back.
    { "assert-audio-frequency",
      "<assert-audio-frequency filename='r.wav' minHz='430' maxHz='450'"
      " frameCount='48000' channel='0'/>" },
    // minDiffRms is written only when >= 0 (the default -1 means "not checked"),
    // so the fixture gives it a real value to keep it in the audit.
    { "assert-channels-differ",
      "<assert-channels-differ filename='r.wav' channelA='0' channelB='3'"
      " minRmsDelta='0.250000' minDiffRms='0.100000' startFrame='4000'"
      " frameCount='4000'/>" },
    // expectSilence is only written when true, so it stays out of the fixture.
    { "assert-source-position",
      "<assert-source-position filename='r.wav' startFrame='40960'"
      " frameCount='4096' channel='0' expectSourceFrame='40960'"
      " tolerance='2048' expectSourceFrameAny='0,40960' minConfidence='5.000'/>" },
    { "assert-file-contains",
      "<assert-file-contains path='p.qxp' text='uid=&apos;x&apos;'"
      " absent='true'/>" },
    // maxReportedDiffs is written only when it is NOT the default 8, so the
    // fixture gives it another value to keep it in the audit.
    { "assert-file-identical",
      "<assert-file-identical actual='r.wav' expected='../ref.wav'"
      " maxReportedDiffs='3'/>" },
    // Same shape: maxPages/maxBytes are written only when >= 0.
    { "report-page-memory",
      "<report-page-memory label='after render' maxPages='4096'"
      " maxBytes='1073741824'/>" },
    // durationSec is written only when > 0, so a fixture must give it one — it
    // is the only optional attribute `render` has.
    { "render",
      "<render filename='r.wav' format='wav' quality='10'"
      " durationSec='2.5'/>" },
    // minFrames is only written when non-zero, so a fixture must give it one.
    { "dump-playback-capture",
      "<dump-playback-capture filename='playback.wav' minFrames='315392'/>" },
    { "assert-sidecar",
      "<assert-sidecar aspect='onsets' minRecords='1' maxRecords='4'"
      " expectExists='true'/>" },
    // All three slip attributes at once: each is written only when present,
    // so a fixture naming just one would leave the other two untested.
    { "slip-clip",
      "<slip-clip clip='0,1' startOffset='48000' srcStart='24000'"
      " loopStart='12000'/>" },

    // --- the five plugin verbs (proposal 08) --------------------------------
    // insert-plugin/remove-plugin carry the opaque plugin STATE chunk since M5:
    // the base64 below is the 'TWCP' frame the CLAP backend writes.
    { "insert-plugin",
      "<insert-plugin trackPath='0' slotIndex='1' format='clap'"
      " uid='tw.test.clap.stereoskew' name='Skew' vendor='Smaragd'"
      " path='twtestclap.clap' nIn='2' nOut='2'"
      " state='VFdDUAEAAAAAAAAAAAAAQAAAAAAAAAAA'/>" },
    { "remove-plugin",
      "<remove-plugin trackPath='0' slotIndex='1' format='clap'"
      " uid='tw.test.clap.stereoskew' name='Skew' vendor='Smaragd'"
      " path='twtestclap.clap' nIn='2' nOut='2'"
      " state='VFdDUAEAAAAAAAAAAAAAQAAAAAAAAAAA'/>" },
    { "set-plugin-bypass",
      "<set-plugin-bypass trackPath='0' slotIndex='2' bypassed='true'/>" },
    { "reorder-plugin",
      "<reorder-plugin trackPath='0' fromIndex='2' toIndex='0'/>" },
    { "set-plugin-param",
      "<set-plugin-param trackPath='0' slotIndex='1' paramId='7'"
      " value='2.5'/>" },

    // --- the clip verbs that became path-addressed --------------------------
    // add-sample/remove-sample used to carry a top-level `trackIndex`, which is
    // why a clip on a NESTED (grouped) track could not be named at all. The
    // fixtures use a two-level path deliberately: a one-element path would
    // round-trip even through the old int field.
    { "add-sample",
      "<add-sample trackPath='0,1' filePath='x.wav' timePos='96000'/>" },
    { "remove-sample",
      "<remove-sample trackPath='0,1' clipIndex='1' filePath='x.wav'"
      " timePos='96000'/>" },

    // --- the lane flag verbs ------------------------------------------------
    // Two-level paths deliberately: a top-level-only `trackIndex` (which is
    // what solo/mute effectively had before there was any verb at all) would
    // round-trip a one-element path just fine and prove nothing.
    { "set-track-solo",  "<set-track-solo trackPath='1,0' solo='1'/>" },
    { "set-track-mute",  "<set-track-mute trackPath='1,1' muted='1'/>" },
    { "set-track-volume",
      "<set-track-volume trackPath='1,0' volume='-6'/>" },

    // --- the M5 plugin-UI verbs --------------------------------------------
    { "assert-plugin-strip",
      "<assert-plugin-strip trackIndex='0' slotCount='2' slotIndex='1'"
      " contains='state=Missing' absent='reload=0'/>" },
    { "plugin-editor-set-param",
      "<plugin-editor-set-param trackIndex='0' slotIndex='0' paramId='1'"
      " value='0.25'/>" },

    // --- the multi-track selection verbs ------------------------------------
    // Two-level paths and a modifier COMBINATION deliberately: the modifier
    // string is re-derived from parsed flags on write, so a single-modifier
    // fixture would not catch a dropped one.
    { "select-track",
      "<select-track trackPath='1,0' modifiers='ctrl+shift'/>" },
    { "track-head-toggle",
      "<track-head-toggle trackPath='1,0' control='solo' on='0'/>" },
    { "drag-track",
      "<drag-track trackPath='1,0' targetRow='2' mode='before'/>" },
    { "assert-track-selection",
      "<assert-track-selection paths='0;1,0' primary='1,0'/>" },
};

const char *fixtureFor(const QString &verb)
{
    for (const Fixture &f : kFixtures) {
        if (verb == QLatin1String(f.verb)) return f.xml;
    }
    return nullptr;
}

// Two attribute values are equivalent if they are the same string, or the same
// number. Writers normalize ("0.09" is written back as "0.090000"), and that is
// not a round-trip defect.
bool attrEquivalent(const QString &a, const QString &b)
{
    if (a == b) return true;
    bool okA = false, okB = false;
    const double da = a.toDouble(&okA);
    const double db = b.toDouble(&okB);
    return okA && okB && std::fabs(da - db) <= 1e-12 * (1.0 + std::fabs(da));
}

void writeTo(SAction *action, QDomDocument &doc, QDomElement &elem)
{
    elem = doc.createElement(action->name());
    if (action->formatVersion() != 1) {
        elem.setAttribute("version", action->formatVersion());
    }
    action->writeXml(elem);
    doc.appendChild(elem);
}

}  // namespace

bool testActionRoundTrip(const QString &actionName, QString &error)
{
    SActionRegistry &registry = SActionRegistry::instance();

    // Create a default instance.
    SAction *action1 = registry.create(actionName);
    if (!action1) {
        error = QString("Failed to create action: %1").arg(actionName);
        return false;
    }

    // Load the representative fixture, when there is one.
    QDomDocument fixtureDoc;
    QDomElement fixtureElem;
    if (const char *xml = fixtureFor(actionName)) {
        const QDomDocument::ParseResult parsed =
            fixtureDoc.setContent(QString::fromLatin1(xml));
        if (!parsed) {
            error = QString("Fixture for %1 is not well-formed XML at %2:%3: %4")
                        .arg(actionName)
                        .arg(parsed.errorLine)
                        .arg(parsed.errorColumn)
                        .arg(parsed.errorMessage);
            delete action1;
            return false;
        }
        fixtureElem = fixtureDoc.documentElement();
        if (!action1->readXml(fixtureElem, action1->formatVersion())) {
            error = QString("%1 rejected its own fixture").arg(actionName);
            delete action1;
            return false;
        }
    }

    // Serialize (action 1).
    QDomDocument doc1;
    QDomElement elem1;
    writeTo(action1, doc1, elem1);

    // Every attribute the fixture declared must have survived readXml+writeXml.
    if (!fixtureElem.isNull()) {
        const QDomNamedNodeMap attrs = fixtureElem.attributes();
        for (int i = 0; i < attrs.count(); ++i) {
            const QDomNode node = attrs.item(i);
            const QString key = node.nodeName();
            const QString want = node.nodeValue();
            if (!elem1.hasAttribute(key)) {
                error = QString("%1: attribute '%2' from the fixture is not "
                                "written back at all (readXml ignores it?)")
                            .arg(actionName, key);
                delete action1;
                return false;
            }
            const QString got = elem1.attribute(key);
            if (!attrEquivalent(want, got)) {
                error = QString("%1: attribute '%2' round-tripped as '%3', "
                                "expected '%4'")
                            .arg(actionName, key, got, want);
                delete action1;
                return false;
            }
        }
    }

    // Deserialize to a new action (action 2).
    SAction *action2 = registry.createFromXml(elem1);
    if (!action2) {
        error = QString("Failed to deserialize action from XML: %1").arg(actionName);
        delete action1;
        return false;
    }

    // Serialize action 2 to XML.
    QDomDocument doc2;
    QDomElement elem2;
    writeTo(action2, doc2, elem2);

    const QString xml1Str = doc1.toString();
    const QString xml2Str = doc2.toString();

    delete action1;
    delete action2;

    if (xml1Str != xml2Str) {
        error = QString("Round-trip mismatch for %1:\nFirst:  %2\nSecond: %3")
            .arg(actionName, xml1Str, xml2Str);
        return false;
    }

    return true;
}

// Audit all registered actions for round-trip correctness.
// Exit code: 0 = all pass, 1 = any failure.
int main()
{
    SActionRegistry &registry = SActionRegistry::instance();
    QStringList names = registry.knownNames();

    std::cout << "Testing " << names.size() << " actions for round-trip correctness...\n";

    QStringList failures;

    // A fixture for a verb nobody registers is a stale fixture: it silently
    // stops testing anything, which is how this file rotted the first time.
    for (const Fixture &f : kFixtures) {
        if (!names.contains(QLatin1String(f.verb))) {
            failures.append(QString("Stale fixture: no registered verb '%1'")
                                .arg(QLatin1String(f.verb)));
        }
    }

    for (const QString &name : names) {
        QString error;
        if (!testActionRoundTrip(name, error)) {
            failures.append(error);
            std::cout << "FAIL: " << name.toStdString() << "\n";
        } else {
            std::cout << "PASS: " << name.toStdString()
                      << (fixtureFor(name) ? "  (fixture)" : "") << "\n";
        }
    }

    std::cout << "\n";

    if (failures.isEmpty()) {
        std::cout << "All " << names.size() << " actions passed round-trip test.\n";
        return 0;
    } else {
        std::cout << failures.size() << " failures:\n\n";
        for (const QString &failure : failures) {
            std::cout << failure.toStdString() << "\n\n";
        }
        return 1;
    }
}
