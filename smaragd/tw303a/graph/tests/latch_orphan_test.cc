// tw/graph module test: A PLUG MAY OUTLIVE ITS LATCH, and must know it.
//
// QBX-155. twLatch's own comment has always said this is intended --
//
//     "The latch owns its outputs. Consumers hold a shared_ptr to the plug they
//      read from (twComponent::pInputPlugs_) ... the object frees once the last
//      consumer releases it."
//
// -- but the plug's back-reference was a `twLatch & parentLatch`, and the latch
// dies with its component (twComponent::pOutputLatches_) while ~twComponent
// severs no consumer. So a consumer routinely held a plug whose latch was
// freed, and twComponent::setInput then ran
//
//     oldPlug->getParentLatch().deleteOutput( oldPlug );
//
// into reused memory. setInput even TESTED `&getParentLatch() == nullptr`, which
// a reference can never satisfy: the guard was dead code that read as a real one.
//
// WHY A MODULE TEST AND NOT A .qxa CASE. The app-level route to this state is a
// wrapped take column deleted and restored, which is a cut-module change that
// must land AFTER this one. A gate that needed that change could not protect
// this one on its own, so the lifetime is exercised here, directly: build a
// producer, hand its plug to a consumer, destroy the producer, and ask.
//
// Sabotaged to check it bites: restoring ~twLatch's original empty body (the
// `// FIXME: Empty the latch output list.` it carried) leaves the plug claiming
// a live latch and this test SEGFAULTs on the rewire below.
#include "tw/graph/twcomponent.h"
#include "tw/graph/twlatch.h"
#include "tw/graph/tw303aenv.h"

#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// A producer with one streaming output latch, and nothing else. It never
// renders, because none of this is about rendering.
class Producer : public twComponent {
public:
    explicit Producer(tw303aEnvironment &e) : twComponent(e) {}
    void reset() override {}
    void createOutputLatches() override {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs()  const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t)  const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "out"; }

    // pOutputLatches_ is protected and has no public accessor, so only a
    // derived class can hand the test the latch to take an output from.
    twLatch *testLatch() { return pOutputLatches_[0].get(); }
};

// A consumer with one input, so setInput(0, ...) is the real production path.
class Consumer : public twComponent {
public:
    explicit Consumer(tw303aEnvironment &e) : twComponent(e) {}
    void reset() override {}
    void createOutputLatches() override {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs()  const override { return 1; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t)  const override { return "in"; }
    const char *getOutputName(idx_t) const override { return "out"; }
};

int main()
{
    tw303aEnvironment env;

    // --- 1. The plug survives its latch, and SAYS so ----------------------
    {
        auto consumer = std::make_shared<Consumer>(env);
        consumer->init();

        twLatchOutput *plug = nullptr;
        {
            auto producer = std::make_shared<Producer>(env);
            producer->init();
            plug = producer->testLatch()->addOutput();
            CHECK(plug != nullptr, "the latch hands out an output");

            consumer->setInput(0, plug);
            CHECK(consumer->getInputPlug(0) == plug,
                  "the consumer reads from the producer's plug");
            CHECK(plug->latchOrNull() == producer->testLatch(),
                  "and the plug names that very latch while it lives");
            CHECK(!plug->isOrphaned(),
                  "a plug with a live latch is not orphaned");
        }
        // The producer is gone. The PLUG is not: the consumer's
        // pInputPlugs_[0] is a real shared_ptr, which is the whole design --
        // an audio-thread snapshot must stay valid across a disconnect.
        CHECK(plug->isOrphaned(),
              "the plug OUTLIVED its latch and reports itself orphaned");
        CHECK(plug->latchOrNull() == nullptr,
              "latchOrNull() answers null rather than a freed address");

        // --- 2. Rewiring away from a dead plug: the actual crash ----------
        //
        // This is the exact call that SEGFAULTed, reached from
        // SApplication::rewireSpeaker at project teardown.
        consumer->setInput(0, nullptr);
        CHECK(consumer->getInputPlug(0) == nullptr,
              "the consumer drops an orphaned plug instead of writing into it");
    }

    // --- 3. An orphaned plug READS AS SILENCE -----------------------------
    //
    // The ZOMBIE contract one level down: a component tearing down outputs
    // silence, and a plug whose latch is gone is in that position. 0 frames
    // and a null page are what every caller already handles.
    {
        auto consumer = std::make_shared<Consumer>(env);
        consumer->init();

        twLatchStreamingOutput *plug = nullptr;
        {
            auto producer = std::make_shared<Producer>(env);
            producer->init();
            plug = dynamic_cast<twLatchStreamingOutput *>(
                       producer->testLatch()->addOutput());
            CHECK(plug != nullptr, "the streaming latch hands out a streaming output");
            consumer->setInput(0, plug);
        }
        CHECK(plug->isOrphaned(), "orphaned after its producer went");

        std::vector<sample_t> dest(256, 42.0f);
        const length_t got = plug->readStreamingData(dest.data(), 256);
        CHECK(got == 0, "an orphaned plug reads 0 frames");
        CHECK(dest[0] == 42.0f,
              "...and writes NOTHING, so the caller's buffer is untouched");
        CHECK(plug->fetchPage(0) == nullptr,
              "and offers no page, which already means 'render silence'");

        // getFormat() must answer rather than fault: the consumer asking is
        // about to be rewired away from this plug.
        (void) plug->getFormat();
        CHECK(true, "getFormat() on an orphaned plug does not fault");

        consumer->setInput(0, nullptr);
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nall ok\n");
    return 0;
}
