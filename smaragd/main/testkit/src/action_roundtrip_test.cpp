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
    // Same shape (minDiffRms only written when >= 0), plus a clip path, which a
    // default instance does not have.
    { "assert-clip-channels",
      "<assert-clip-channels clip='0,0' position='4096' expectChannels='2'"
      " channelA='0' channelB='1' minRmsDelta='0.150000'"
      " minDiffRms='0.150000'/>" },
    // Proposal 36 B4. minDiffRms/minRms are only written when >= 0, and the
    // driver is always written (its default is meaningful: "scheduler" and
    // "pull" exercise two different halves of the engine).
    { "assert-track-channels",
      "<assert-track-channels trackPath='0' position='65536' expectChannels='2'"
      " channelA='0' channelB='1' minRmsDelta='0.150000'"
      " minDiffRms='0.150000' minRms='0.100000' driver='pull'/>" },
    { "assert-master-sums",
      "<assert-master-sums position='65536' tolerance='0.001' stride='97'"
      " minRms='0.010000'/>" },
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
      " maxReportedDiffs='3' startFrame='48000' frameCount='96000'/>" },
    // Same shape: maxPages/maxBytes are written only when >= 0.
    { "report-page-memory",
      "<report-page-memory label='after render' maxPages='4096'"
      " maxBytes='1073741824'/>" },
    // durationSec is written only when > 0, so a fixture must give it one — it
    // is the only optional attribute `render` has.
    { "render",
      "<render filename='r.wav' format='wav' quality='10'"
      " durationSec='2.5'/>" },
    // level is written only when non-empty; minCount/maxCount always are.
    { "assert-log",
      "<assert-log contains='SFutureThing' minCount='2' maxCount='4'"
      " level='warn'/>" },
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
    // The INSTRUMENT flag (proposal 37 P3b) is written only when true, so it
    // needs a fixture of its own: the effect row above cannot cover an
    // attribute it correctly never emits.
    { "insert-plugin",
      "<insert-plugin trackPath='0' slotIndex='0' format='clap'"
      " uid='tw.test.clap.sine' name='Sine' vendor='Smaragd'"
      " path='twtestclap.clap' nIn='0' nOut='2' isInstrument='true'/>" },
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
    // A non-empty name, deliberately: the default is the empty string, which an
    // unread attribute also produces.
    { "set-track-name",
      "<set-track-name trackPath='1,1' name='Drum bus'/>" },

    // A NON-default width, deliberately: the default (2) is what an unread
    // attribute also produces, so a fixture of 2 would round-trip through a
    // readXml that ignored the attribute entirely.
    { "set-project-channels",
      "<set-project-channels channels='6'/>" },

    // --- the M5 plugin-UI verbs --------------------------------------------
    { "assert-plugin-strip",
      "<assert-plugin-strip trackIndex='0' slotCount='2' slotIndex='1'"
      " contains='state=Missing' absent='reload=0'/>" },
    { "plugin-editor-set-param",
      "<plugin-editor-set-param trackIndex='0' slotIndex='0' paramId='1'"
      " value='0.25'/>" },

    // --- the P3b instrument verb -------------------------------------------
    { "assert-instrument-slot",
      "<assert-instrument-slot trackPath='0,1' trackIndex='0' present='1'"
      " uid='tw.native.303' format='tw' mode='MonoSpread' state='Active'"
      " minTailFrames='24000' maxTailFrames='24000' hasFeed='1'/>" },

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

    // --- the event-clip verbs (proposal 37 P1) ------------------------------
    // Two-level paths throughout, for the reason above. `duration` here is in
    // TICKS (the clip's own unit); everything else positional is frames.
    { "insert-midi-clip",
      "<insert-midi-clip trackPath='1,0' timePos='96000' duration='3840'"
      " name='Verse'/>" },
    { "import-midi-file",
      "<import-midi-file trackPath='1,0' filePath='../song.mid'"
      " timePos='96000' mode='merged' newTracks='0'/>" },
    // Both addressing attributes at once: each is written only when present,
    // so a fixture naming one would leave the other untested.
    { "export-midi-file",
      "<export-midi-file filePath='out.mid' clip='1,0' trackPath='1'"
      " type='0'/>" },
    { "add-note",
      "<add-note clip='1,0' tick='960' dur='480' key='64' velocity='90'"
      " channel='2' releaseVelocity='32' take='1' broadcast='0'/>" },
    { "remove-note",
      "<remove-note clip='1,0' tick='960' key='64' channel='2' take='1'"
      " broadcast='0'/>" },
    // The batch verbs carry CHILD elements, so the round trip has to survive
    // the children as well as the attributes.
    { "set-notes",
      "<set-notes clip='1,0' take='1' broadcast='0'>"
      "<n tick='0' dur='480' key='60' velocity='100' channel='0'/>"
      "<n tick='960' dur='240' key='67' velocity='80' channel='0'/>"
      "</set-notes>" },
    { "set-events",
      "<set-events clip='1,0' take='1'>"
      "<e k='cc' t='480' ch='0' p='7' v='64'/>"
      "<e k='note' t='0' d='480' ch='0' key='60' v='100'/>"
      "</set-events>" },
    { "add-event",
      "<add-event clip='1,0' kind='cc' tick='480' channel='2' key='64' p='7'"
      " v='64' v2='2' text='hello' blob='07a120' take='1'/>" },
    { "remove-event",
      "<remove-event clip='1,0' kind='cc' tick='480' channel='2' p='7'"
      " take='1'/>" },
    { "quantize-notes",
      "<quantize-notes clip='1,0' grid='1/8t' strength='0.75' swing='0.15'"
      " take='1' broadcast='0'/>" },
    { "set-midi-cut",
      "<set-midi-cut clip='1,0' transpose='-12' velocityScale='0.5'"
      " channel='3' take='1' broadcast='0'/>" },
    // --- automation (proposal 37 P5) ------------------------------------
    // `owner` is an index path; the TARGET's space decides whether it names a
    // lane, a slot's track, or a placement — so every row below carries a
    // target and, where it is a slot, a slotIndex.
    { "add-automation-lane",
      "<add-automation-lane owner='0' target='self:Volume' mode='read'>"
      "<p t='0' v='-60' c='linear'/>"
      "<p t='192000' v='0' c='linear'/>"
      "</add-automation-lane>" },
    { "remove-automation-lane",
      "<remove-automation-lane owner='0,1' target='cut:Gain' take='1'/>" },
    { "set-automation-mode",
      "<set-automation-mode owner='0' target='param:7' slotIndex='2'"
      " mode='latch'/>" },
    { "add-automation-point",
      "<add-automation-point owner='0' target='self:Volume' time='96000'"
      " value='-30' curve='exp' tension='2.5'/>" },
    { "remove-automation-point",
      "<remove-automation-point owner='0' target='self:Muted' time='48000'"
      " value='1'/>" },
    { "move-automation-point",
      "<move-automation-point owner='0' target='param:0' slotIndex='1'"
      " time='96000' value='2' toTime='120000' toValue='1.5'/>" },
    { "set-automation-points",
      "<set-automation-points owner='0,1' target='cut:Gain' from='0'"
      " to='192000' take='1'>"
      "<p t='0' v='1' c='linear'/>"
      "<p t='192000' v='0' c='linear'/>"
      "</set-automation-points>" },
    { "assert-automation-value",
      "<assert-automation-value owner='0' target='self:Volume' time='96000'"
      " value='-30' tolerance='1e-9' mode='read' pointCount='2'/>" },
    { "set-tempo", "<set-tempo bpm='132.5'/>" },
    { "set-link-timebase",
      "<set-link-timebase clip='1,0' timebase='time'/>" },
    { "set-track-midi-routing",
      "<set-track-midi-routing trackPath='1,0' routing='parent'/>" },
    // set-track-midi-output is ABSOLUTE, so every attribute is always written
    // (an omitted channel would otherwise read back as "as authored" and undo
    // would restore the wrong thing).
    { "set-track-midi-output",
      "<set-track-midi-output trackPath='1,0' port='capture' channel='2'"
      " offsetMs='-200'/>" },

    // Proposal 21 L1b. All three are ABSOLUTE, so every attribute is always
    // written and every one of them must survive the round trip: an omitted
    // `armed` would read back as 0 and undo would un-arm a track the user
    // never touched.
    { "arm-track", "<arm-track trackPath='1,0' armed='1'/>" },
    { "set-track-input",
      "<set-track-input trackPath='1,0' input='audio:default:3'/>" },
    { "set-monitor-mode", "<set-monitor-mode trackPath='1,0' mode='on'/>" },

    // Audio recording (proposal 21 L3b). record-start / record-stop carry no
    // attributes at all, so they need no fixture row; assert-recorded-clip
    // writes only what is non-default, so only the attributes below may be
    // declared here.
    { "assert-recorded-clip",
      "<assert-recorded-clip trackPath='1,0' clips='1' takes='3' passes='3'"
      " startFrame='48000' startTolerance='0' durationFrames='48000'"
      " durationTolerance='0' minDurationFrames='24000'"
      " inputLatencyFrames='4800' outputLatencyFrames='1024'"
      " userOffsetFrames='-960' compensationFrames='-6784' trimmedFrames='0'"
      " growing='true' previewNonEmpty='true' sourceAtStartFrame='0'"
      " sourceTolerance='2048'/>" },
    { "place-recording",
      "<place-recording trackPath='0' filePath='x.wav' timePos='96000'"
      " srcOffset='48000' length='24000'/>" },

    // MIDI recording (proposal 21 L4). Both verbs carry CHILD elements, so the
    // round trip has to survive the events as well as the attributes; the
    // children are `<e .../>` and not `<n .../>` because a recorded pass
    // carries CCs as well as notes and the note spelling would read a CC back
    // as a NoteOn. record-start / record-stop still carry no attributes.
    { "add-midi-take",
      "<add-midi-take clip='1,0' index='2' activate='0' name='Take 3'"
      " lengthTicks='3840'>"
      "<e k='note' t='0' d='480' ch='0' key='60' v='100'/>"
      "<e k='note' t='960' d='240' ch='0' key='67' v='80'/>"
      "</add-midi-take>" },
    { "place-midi-recording",
      "<place-midi-recording trackPath='1,0' timePos='96000'"
      " durationTicks='3840' mode='overdub' quantize='1/16' name='Rec'>"
      "<e k='note' t='0' d='480' ch='0' key='60' v='100'/>"
      "<e k='cc' t='480' ch='0' p='7' v='64'/>"
      "</place-midi-recording>" },
    { "assert-midi-recorded",
      "<assert-midi-recorded trackPath='1,0' clips='1' takes='3' minTakes='2'"
      " passes='3' minPasses='2' notes='8' minNotes='4' events='2'"
      " startFrame='48000' startTolerance='0' durationFrames='96000'"
      " durationTolerance='0' mode='new-take' quantize='1/16'"
      " contains='rec=off'/>" },

    // --- the event test verbs ----------------------------------------------
    // clip AND trackPath, kind, contains and velocityTolerance are each
    // written only when set, so the fixture sets every one of them.
    { "assert-midi-events",
      "<assert-midi-events scope='feed' clip='1,0' trackPath='1' take='1'"
      " kind='noteoff-synth' at='24000' tolerance='64' key='60'"
      " velocity='100' velocityTolerance='2' channel='0' count='2'"
      " minCount='1' maxCount='3' startFrame='48000' frameCount='96000'"
      " contains='dur=48000'/>" },
    { "assert-midi-file",
      "<assert-midi-file filename='out.mid' trackCount='3' noteCount='6'"
      " eventCount='10' firstTick='0' ppq='960' format='1'/>" },
    // --- the event-editor verbs (proposal 37 P4) ---------------------------
    // Every attribute is written unconditionally by these four, so a fixture
    // declaring all of them keeps all of them in the audit.
    // The PLAY mode of proposal 21 L2 is folded in rather than given a second
    // fixture: the lookup returns the FIRST match for a verb, so one element
    // has to carry every attribute. `release` is absent because writeXml omits
    // it when false, which is the rule the header above states.
    { "virtual-key",
      "<virtual-key key='67' velocity='90' durationTicks='480' hold='1'"
      " durationMs='250'/>" },
    // The live-instrument assertions (proposal 21 L2).
    { "assert-audio-onset",
      "<assert-audio-onset filename='cap.wav' channel='1' startFrame='4096'"
      " threshold='0.05' window='128' minFrame='0' maxFrame='12288'"
      " afterMidiIn='1'/>" },
    { "assert-render-policy",
      "<assert-render-policy liveThreadRefusals='0' liveOwnedRefusals='64'"
      " minLiveOwnedRefusals='1'/>" },
    // Transport polish (proposal 21 L5). Every optional attribute is present,
    // because each of them is written only when it was given.
    { "assert-metronome-clicks",
      "<assert-metronome-clicks filename='met.wav' channel='1'"
      " startFrame='4096' threshold='0.05' window='128' minGapFrames='4800'"
      " count='8' minCount='4' maxCount='12' accentEvery='4'"
      " intervalFrames='24000' toleranceFrames='1024'"
      " accentRatio='1.8' silenceMaxRms='0.01' firstFrame='2048'"
      " firstTolerance='512'/>" },
    { "set-count-in", "<set-count-in bars='2'/>" },
    { "set-pre-roll",  "<set-pre-roll bars='1'/>" },
    // edge='end' deliberately: with an empty edge and lane='notes', readXml
    // DEFAULTS toKey to key (a move needs a destination, a resize does not),
    // which would hide a dropped toKey.
    { "drag-note",
      "<drag-note clip='1,0' tick='960' key='64' channel='2' toTick='1920'"
      " toKey='67' edge='end' lane='notes' toValue='90'/>" },
    { "assert-event-editor",
      "<assert-event-editor clip='1,0' kind='pianoroll' contains='notes=2'"
      " absent='empty=1' grabPng='roll.png' grabWidth='640'"
      " grabHeight='320'/>" },
    { "assert-track-head",
      "<assert-track-head trackPath='1,0' headHeight='132'"
      " contains='density=Full' absent='I=1' grabPng='head.png'"
      " grabWidth='160' grabHeight='160'/>" },

    // --- the automation UI verbs (proposal 37 P6) ---------------------------
    // `take` rather than `slotIndex` here, because a `cut:` target is the one
    // that has takes; the slot half is covered by the write-tick row below.
    // toTime/toValue are written only when the element carried either, so the
    // fixture must set both or the pair would legitimately not come back.
    { "drag-automation-point",
      "<drag-automation-point owner='0,1' target='cut:Gain' take='1'"
      " time='48000' value='0.25' toTime='96000' toValue='0.75'"
      " modifiers='ctrl+alt'/>" },
    // `time` and `release` are both written only when set, so both are here.
    { "automation-write-tick",
      "<automation-write-tick owner='0' target='param:1' slotIndex='2'"
      " value='0.5' time='24000' release='1'/>" },
    // set-lane-view writes each half only when it was asked for, and the
    // automation half needs its three attributes together.
    { "set-lane-view",
      "<set-lane-view trackHeight='140' topRow='2' laneScaleRow='0'"
      " laneScale='2.5' toggleTakesRow='1' automationTrack='0,1'"
      " automationTarget='param:3' automationSlot='2' showAutomation='1'"
      " clipEnvelopes='1'/>" },
    { "assert-lane-alignment",
      "<assert-lane-alignment grabPng='lanes.png' grabWidth='900'"
      " grabHeight='700'/>" },
    // --- the folder-sum preview verbs (proposal 39) -------------------------
    // assert-envelope writes trackPath / snapshot / compareTo only when set,
    // and the childSum half of it is unreachable from a default instance.
    { "assert-envelope",
      "<assert-envelope clip='0,0' trackPath='0,1' mode='childSum'"
      " start='48000' length='96000' width='32' column='4' min='-25' max='25'"
      " tolerance='2' expectEmpty='false' snapshot='a' compareTo='b'/>" },
    // Every grab attribute is written only when given, `contains` likewise.
    { "assert-lane-overlay",
      "<assert-lane-overlay trackPath='0,1' expectOverlay='false'"
      " minPixels='200' grabWidth='900' grabHeight='700'"
      " grabPng='folder.png' contains='row=0'/>" },

    // timebase is written only when non-empty.
    // --- the MIDI-out verbs (proposal 37 P7b) -------------------------------
    // port/kind are written only when non-empty and `at` only when it was
    // given, so the fixture sets all three plus every numeric attribute.
    // maxLagMs (proposal 21 L2 AC5) is written only when >= 0, so it belongs
    // in the one fixture rather than in a second element.
    { "assert-midi-out",
      "<assert-midi-out port='capture' kind='noteon' channel='2' key='60'"
      " cc='7' value='100' at='-9600' tolerance='2048' count='1'"
      " minCount='1' maxCount='2' index='0' maxLagMs='5'/>" },
    // minCount is written only when non-zero.
    { "dump-midi-capture",
      "<dump-midi-capture filename='midi_out.txt' minCount='6'/>" },
    { "assert-midi-options",
      "<assert-midi-options contains='backend=capture' absent='virtual=no'"
      " outputPorts='1' inputPorts='1'/>" },
    { "set-option", "<set-option key='midi/outOffsetMs' value='120'/>" },
    // --- the MIDI-in verbs (proposal 21 L0) ---------------------------------
    // The field form and the raw-bytes form are exclusive (bytes= suppresses
    // the fields on write), so the fixture uses the field form plus cc, and
    // atFrame is written only when it was given.
    { "midi-in-event",
      "<midi-in-event kind='cc' key='60' velocity='100' channel='2' cc='7'"
      " atFrame='48000' timeoutMs='5000'/>" },
    { "midi-in-replay",
      "<midi-in-replay filePath='perf.mid' track='1' channel='3'"
      " startFrame='96000' timeoutMs='5000'/>" },
    { "wait-ms", "<wait-ms ms='3600'/>" },

    { "assert-clip-window",
      "<assert-clip-window clip='1,0' startTime='96000' duration='192000'"
      " loopLength='24000' startOffset='4800' timebase='beats' take='1'/>" },
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
