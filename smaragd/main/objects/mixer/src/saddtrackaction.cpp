#include "app/objects/mixer/saddtrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/sappcontext.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <QString>

// Generate an absurd-but-memorable track name, reddit-username style
// (Adjective + Noun, CamelCase) — with a few thinking-interjection words thrown
// into the adjective bag. A process-wide counter walks distinct combinations so
// consecutive new tracks don't collide; after the pool wraps, a number is
// appended.
//
// The counter varies the NOUN (the end of the name) fastest and the ADJECTIVE
// (the front) only once every nN names, deliberately — the track-control
// column is narrow, and when it elides a long name it keeps the front and
// drops the tail, or vice versa depending on the platform style, but either
// way a run of consecutive tracks that only ever changed at the front reads
// as one repeated name once the differing part is the part that gets cut.
// Disambiguating at the END first keeps consecutively-created tracks visually
// distinct for far longer before ever touching the front of the name.
static QString generateTrackName()
{
    static const char *adjs[] = {
        "Grumpy","Soggy","Velvet","Sneaky","Cosmic","Wobbly","Spicy","Mellow",
        "Funky","Drowsy","Glitchy","Plucky","Rusty","Zesty","Bouncy","Cranky",
        "Dapper","Feral","Jolly","Quirky","Snazzy","Turbo","Mighty","Peppy",
        "Nimble","Goofy","Sublime","Hasty",
        "Hmm","Aha","Whoa","Yikes","Oof"   // the noises one makes while thinking
    };
    static const char *nouns[] = {
        "Narwhal","Waffle","Otter","Pretzel","Wombat","Kazoo","Gizmo","Badger",
        "Noodle","Penguin","Mango","Goblin","Walrus","Muffin","Llama","Yeti",
        "Cactus","Raccoon","Pickle","Hedgehog","Squid","Biscuit","Ferret","Comet",
        "Bagel","Moose","Gecko","Turnip","Falcon","Doodle"
    };
    const int nA = (int)(sizeof(adjs)/sizeof(adjs[0]));
    const int nN = (int)(sizeof(nouns)/sizeof(nouns[0]));
    static int counter = 0;
    int n = counter++;
    // Noun (end) varies on every call; adjective (front) only every nN calls.
    QString name = QString(adjs[(n / nN) % nA]) + nouns[n % nN];
    int cycle = n / (nA * nN);
    if (cycle > 0) name += QString::number(cycle + 1);   // ...Otter2 after wrap
    return name;
}

SAddTrackAction::SAddTrackAction(int index)
    : index_(index)
{
}

SApplyResult SAddTrackAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // Get the root SStdMixer.
    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    // Create a new track, give it a fun name, and append it (insertTrack is
    // append-only). Name before insert so the track control shows it on creation.
    STrack *track = new STrack(project);
    track->setSName(generateTrackName());
    mixer->insertTrack(*track);
    int landing = mixer->getNTracks() - 1;

    // Honour a requested index by moving the appended track into place
    // (-1 or out-of-range = leave at the end).
    int actualIndex = (index_ < 0 || index_ > landing) ? landing : index_;
    if (actualIndex != landing) {
        mixer->reorderTrack(landing, actualIndex);
    }

    // Rewire speaker so new track is audible.
    SAppContext::get().rewireSpeaker();

    // Auto-select the newly added track (c)
    mixer->setSelectedTrack(track);

    // Create inverse action: remove at the same index. add-track always appends
    // at the MIXER's top level, so a one-element path is the whole address.
    SAction *inverse = new SRemoveTrackAction(QList<int>{ actualIndex });

    return {true, inverse};
}

void SAddTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("index", index_);
}

bool SAddTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    index_ = elem.attribute("index", "-1").toInt();
    return true;
}

static const bool s_reg_addtrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("add-track"),
        []{ return new SAddTrackAction; }
    ), true
);
