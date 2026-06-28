# Unified Rendering Architecture V2: Test Plan

**Status:** Design phase  
**Purpose:** Comprehensive unit tests for validation and future extensions  
**Framework:** Google Test (GTest) - C++17, async-ready  
**Scope:** Core freezing engine, page caching, invalidation, thread safety

---

## Test Organization

```
tw303a/test/
├── CMakeLists.txt
├── tw_component_tests.cc          # Page cache, freezing, state
├── tw_invalidation_tests.cc       # Invalidation cascade, dependencies
├── tw_audio_engine_tests.cc       # Playback path, sequential rendering
├── tw_thread_safety_tests.cc      # Concurrent access, deadlocks
└── fixtures/
    ├── mock_component.h           # Simple test component
    ├── audio_comparison.h         # Output correctness validation
    └── test_helpers.h             # Common utilities
```

---

## Test Categories

### 1. Page Cache Tests (tw_component_tests.cc)

**Suite: PageCacheBasic**

```cpp
TEST_F(PageCacheBasic, AllocatePageReturnsNonNull) {
    // getOrAllocatePage() for new position returns allocated page
    auto page = component->getOrAllocatePage(0);
    ASSERT_NE(nullptr, page);
}

TEST_F(PageCacheBasic, ReuseSamePageForSamePosition) {
    // Multiple calls to getOrAllocatePage(pos) return same page (sharing)
    auto page1 = component->getOrAllocatePage(1024);
    auto page2 = component->getOrAllocatePage(1024);
    EXPECT_EQ(page1.get(), page2.get());
}

TEST_F(PageCacheBasic, DifferentPositionsDifferentPages) {
    auto page1 = component->getOrAllocatePage(0);
    auto page2 = component->getOrAllocatePage(256*1024);  // One page later
    EXPECT_NE(page1.get(), page2.get());
}

TEST_F(PageCacheBasic, InvalidateMarksAllPageStale) {
    auto page = component->getOrAllocatePage(0);
    page->validAspects = twAspectAll;
    component->invalidateAllPages();
    EXPECT_EQ(0u, page->validAspects);
}

TEST_F(PageCacheBasic, ReleaseOldPagesFreesMemory) {
    // Allocate 10 pages, release all before position 1000
    for (int i = 0; i < 10; ++i) {
        component->getOrAllocatePage(i * 256 * 1024);
    }
    size_t before = getPageCacheSize(component);
    component->releaseOldPages(1000);
    size_t after = getPageCacheSize(component);
    EXPECT_LT(after, before);
}
```

**Suite: PageCacheStateSnapshotIntegration**

```cpp
TEST_F(PageCacheStateSnapshotIntegration, CaptureAndRestoreState) {
    // Test component can capture state
    component->setSomeState(42);  // Hypothetical stateful component
    auto state = component->captureInternalState();
    EXPECT_NE(typeid(std::any_cast<Component::InternalState>(state)), typeid(void));
    
    component->setSomeState(0);
    component->restoreInternalState(state);
    EXPECT_EQ(42, component->getSomeState());
}

TEST_F(PageCacheStateSnapshotIntegration, PageStoresCapturedState) {
    auto page = component->getOrAllocatePage(0);
    component->setSomeState(99);
    page->internalState = component->captureInternalState();
    
    // Corrupt in-memory state
    component->setSomeState(0);
    // Restore should work
    component->restoreInternalState(page->internalState);
    EXPECT_EQ(99, component->getSomeState());
}
```

---

### 2. Freezing Tests (tw_component_tests.cc)

**Suite: FreezePageSequential**

```cpp
TEST_F(FreezePageSequential, Page0CallsReset) {
    MockComponent mock;
    EXPECT_CALL(mock, reset()).Times(1);
    
    auto page = mock.freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    Mock::VerifyAndClearExpectations(&mock);
}

TEST_F(FreezePageSequential, PageNCallsRestoreState) {
    MockComponent mock;
    auto page0 = mock.freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    EXPECT_CALL(mock, restoreInternalState(_)).Times(1);
    auto page1 = mock.freezePage(256*1024/sizeof(float), nullptr, 0, 0, 48000, page0);
    
    Mock::VerifyAndClearExpectations(&mock);
}

TEST_F(FreezePageSequential, FrozenPageContainsAudio) {
    auto page = testComponent->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    ASSERT_NE(nullptr, page);
    ASSERT_GT(page->validFrames, 0);
    EXPECT_EQ(twAspectAll, page->validAspects);
}

TEST_F(FreezePageSequential, StateChainContinuity) {
    // Page 0: render from reset
    auto page0 = testComponent->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    // Verify page 0 has state snapshot
    EXPECT_FALSE(std::any_cast<MockComponent::InternalState>(page0->internalState).empty);
    
    // Page 1: resume from page 0 state (state chain)
    auto page1 = testComponent->freezePage(256*1024/sizeof(float), nullptr, 0, 0, 48000, page0);
    
    // Audio should be continuous (same synth state)
    EXPECT_AUDIO_CONTINUOUS(page0->samples.back(), page1->samples.front());
}

TEST_F(FreezePageSequential, SharedPageReturned) {
    // Tier 2 Enhancement #5: Multi-consumer page sharing
    auto page1a = testComponent->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    auto page1b = testComponent->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    EXPECT_EQ(page1a.get(), page1b.get());  // Same page object
}
```

---

### 3. Invalidation Tests (tw_invalidation_tests.cc)

**Suite: InvalidationCascade**

```cpp
TEST_F(InvalidationCascade, InvalidateAllPagesMarksStale) {
    auto page = component->getOrAllocatePage(0);
    page->validAspects = twAspectAll;
    
    component->invalidateAllPages();
    
    EXPECT_EQ(0u, page->validAspects);
}

TEST_F(InvalidationCascade, AddDependentRegistersConsumer) {
    auto upstream = createMockComponent();
    auto downstream = createMockComponent();
    
    upstream->addDependent(downstream.get());
    
    auto dependents = upstream->getDependents();  // Test accessor
    EXPECT_EQ(1u, dependents.size());
    EXPECT_EQ(downstream.get(), dependents[0]);
}

TEST_F(InvalidationCascade, InvalidatePropagatesToDependents) {
    // Tier 2 Enhancement #1
    auto upstream = createMockComponent();
    auto downstream = createMockComponent();
    
    upstream->addDependent(downstream.get());
    
    auto page_down = downstream->getOrAllocatePage(0);
    page_down->validAspects = twAspectAll;
    
    // Invalidate upstream
    upstream->invalidateAllPages();
    
    // Downstream should also be invalidated
    EXPECT_EQ(0u, page_down->validAspects);
}

TEST_F(InvalidationCascade, SelectiveCascadeUnaffectedBranches) {
    // Create: A → B → C
    //         A → D (unused)
    auto A = createMockComponent();
    auto B = createMockComponent();
    auto C = createMockComponent();
    auto D = createMockComponent();
    
    A->addDependent(B.get());
    B->addDependent(C.get());
    A->addDependent(D.get());
    
    auto pageC = C->getOrAllocatePage(0);
    auto pageD = D->getOrAllocatePage(0);
    pageC->validAspects = twAspectAll;
    pageD->validAspects = twAspectAll;
    
    // Invalidate A
    A->invalidateAllPages();
    
    // C should be invalidated (A → B → C)
    EXPECT_EQ(0u, pageC->validAspects);
    
    // D should also be invalidated (A → D) in this simple model
    // (Selective invalidation would skip D if it wasn't in the active path)
    EXPECT_EQ(0u, pageD->validAspects);
}

TEST_F(InvalidationCascade, RecursiveCascadeDeep) {
    // Chain: A → B → C → D → E
    auto comps = createComponentChain(5);
    
    for (size_t i = 0; i < comps.size() - 1; ++i) {
        comps[i]->addDependent(comps[i+1].get());
    }
    
    // Invalidate A
    comps[0]->invalidateAllPages();
    
    // All should be invalidated
    for (auto& comp : comps) {
        auto page = comp->getOrAllocatePage(0);
        EXPECT_EQ(0u, page->validAspects);
    }
}
```

---

### 4. Playback Path Tests (tw_audio_engine_tests.cc)

**Suite: PlaybackPathFrozenPages (Tier 1)**

```cpp
TEST_F(PlaybackPathFrozen, PullFrameUseFrozenPages) {
    // Tier 1: AudioEngine uses freezePage() not seekTo()
    
    AudioEngine engine(synthComponent, 48000);
    AudioFrame frame;
    
    // Should not call seekTo()
    EXPECT_CALL(*mockSynth, seekTo(_)).Times(0);
    
    // Should produce audio
    EXPECT_TRUE(engine.pullFrame(frame));
    EXPECT_NE(0.0f, frame.channels[0] + frame.channels[1]);  // Non-silent
}

TEST_F(PlaybackPathFrozen, LoopWrappingMaintainsState) {
    // Tier 1: Loop wrapping preserves reverb state
    
    AudioEngine engine(reverbComponent, 48000);
    engine.setLoopBoundaries(true, 0, 1000);  // 1000-frame loop
    
    // Pull through first loop
    std::vector<AudioFrame> firstLoop;
    for (int i = 0; i < 1000; ++i) {
        AudioFrame frame;
        engine.pullFrame(frame);
        firstLoop.push_back(frame);
    }
    
    // Get tail of first loop
    float reverbTail = firstLoop.back().channels[0];
    
    // Pull a few frames into second loop
    AudioFrame wrapped1, wrapped2;
    engine.pullFrame(wrapped1);  // Position 1000 (= 0, wrapped)
    engine.pullFrame(wrapped2);  // Position 1001 (= 1)
    
    // Reverb should continue (not restart from silence)
    // Tone test: fade should continue, not jump
    // (Exact validation depends on reverb impl)
}

TEST_F(PlaybackPathFrozen, SeekingStillWorks) {
    AudioEngine engine(synthComponent, 48000);
    
    // Seeking should reset state (unavoidable)
    engine.seekTo(5000);  // Arbitrary position
    
    EXPECT_EQ(5000u, engine.currentPosition());
    
    // Should still produce audio
    AudioFrame frame;
    EXPECT_TRUE(engine.pullFrame(frame));
}
```

---

### 5. Thread Safety Tests (tw_thread_safety_tests.cc)

**Suite: ConcurrentPageAccess**

```cpp
TEST_F(ConcurrentPageAccess, MultipleReadersPage) {
    auto page = component->getOrAllocatePage(0);
    page->validAspects = twAspectAll;
    
    std::vector<std::thread> readers;
    std::atomic<int> readCount(0);
    
    // Spawn 8 threads, each reading from page
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&]() {
            std::lock_guard<std::mutex> lock(page->pageMutex);
            auto state = page->internalState;  // Read state
            readCount++;
        });
    }
    
    for (auto& t : readers) t.join();
    
    EXPECT_EQ(8, readCount);  // All completed successfully
}

TEST_F(ConcurrentPageAccess, FreezingAndReading) {
    // One thread freezing, others reading
    std::atomic<bool> freezeDone(false);
    std::atomic<int> readCount(0);
    
    std::thread freezer([&]() {
        auto page = component->freezePage(0, nullptr, 0, 0, 48000, nullptr);
        freezeDone = true;
    });
    
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            std::this_thread::sleep_for(std::chrono::microseconds(100));  // Stagger
            auto page = component->getOrAllocatePage(0);
            if (page && freezeDone) {
                std::lock_guard<std::mutex> lock(page->pageMutex);
                readCount++;
            }
        });
    }
    
    freezer.join();
    for (auto& t : readers) t.join();
    
    EXPECT_GT(readCount, 0);  // At least some reads succeeded
}

TEST_F(ConcurrentPageAccess, ConcurrentInvalidation) {
    // Multiple threads invalidating concurrently (no deadlock)
    std::vector<std::thread> invalidators;
    
    for (int i = 0; i < 4; ++i) {
        invalidators.emplace_back([&]() {
            component->invalidateAllPages();
        });
    }
    
    for (auto& t : invalidators) t.join();  // Should not deadlock
    
    // Verify pages are stale
    auto page = component->getOrAllocatePage(0);
    EXPECT_EQ(0u, page->validAspects);
}

TEST_F(ConcurrentPageAccess, NoDeadlockPagMutexAndOutputPagesMutex) {
    // Lock hierarchy test: outputPagesMutex → pageMutex (no reverse)
    
    // This test verifies the locking order by running in stressful pattern
    std::atomic<bool> error(false);
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            try {
                // Try to trigger both locks in order
                auto page = component->getOrAllocatePage(0);
                component->setPageAsFrozen(0, page);  // Uses outputPagesMutex
                
                {
                    std::lock_guard<std::mutex> lock(page->pageMutex);  // Then pageMutex
                    page->validAspects = twAspectAll;
                }
            } catch (...) {
                error = true;
            }
        });
    }
    
    // Should complete in reasonable time (no deadlock)
    for (auto& t : threads) {
        ASSERT_TRUE(t.joinable());
        t.join();
    }
    
    EXPECT_FALSE(error);
}
```

---

### 6. Integration Tests (tw_integration_tests.cc)

**Suite: AudioQualityIntegration**

```cpp
TEST_F(AudioQualityIntegration, ExportQualityConsistent) {
    // Phase 3 export path produces consistent output
    
    RenderSession session;
    RenderParams params;
    params.format = AudioFormat::WAV;
    params.startTimeSec = 0.0;
    params.endTimeSec = 1.0;
    params.outputPath = "/tmp/test_export.wav";
    
    ASSERT_TRUE(session.start(synthComponent, params, 48000));
    
    // Wait for completion
    while (session.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_FALSE(session.isRunning());
    
    // Verify file exists and is valid
    std::ifstream file(params.outputPath, std::ios::binary);
    EXPECT_TRUE(file.good());
}

TEST_F(AudioQualityIntegration, PlaybackReverbState) {
    // Tier 1: Playback maintains reverb state across loop
    
    auto reverbChain = createReverbComponent();
    AudioEngine engine(reverbChain, 48000);
    engine.setLoopBoundaries(true, 0, 48000);  // 1-second loop @ 48kHz
    
    // Render ~2 seconds (with looping)
    std::vector<float> output;
    AudioFrame frame;
    for (int i = 0; i < 96000; ++i) {  // 2 seconds
        engine.pullFrame(frame);
        output.push_back(frame.channels[0]);
    }
    
    // Verify reverb tail continues (doesn't restart at loop boundary)
    float tailAt48000 = output[48000];  // Just after loop
    float tailAt48001 = output[48001];
    
    // Should be close (reverb doesn't decay instantly)
    EXPECT_LT(std::abs(tailAt48000 - tailAt48001), 0.1f);
}

TEST_F(AudioQualityIntegration, MultiConsumerSharing) {
    // Tier 2 #5: Multiple consumers share reverb pages
    
    auto reverb = createReverbComponent();
    auto reader1 = createSampleReaderComponent(reverb);
    auto reader2 = createSampleReaderComponent(reverb);
    
    // Force freezing
    auto page1_reader1 = reverb->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    auto page1_reader2 = reverb->freezePage(0, nullptr, 0, 0, 48000, nullptr);
    
    // Should be the SAME page (sharing, not duplicate)
    EXPECT_EQ(page1_reader1.get(), page1_reader2.get());
    
    // Verify both readers get identical audio
    EXPECT_AUDIO_EQUAL(page1_reader1->samples, page1_reader2->samples);
}
```

---

## Test Fixtures

### mock_component.h

```cpp
class MockComponent : public twComponent {
public:
    MOCK_METHOD(void, reset, (), (override));
    MOCK_METHOD(std::any, captureInternalState, (), (const, override));
    MOCK_METHOD(void, restoreInternalState, (const std::any&), (override));
    MOCK_METHOD(length_t, renderFrames, 
                (sample_t*, length_t, const sample_t*, length_t, idx_t),
                (override));
    MOCK_METHOD(length_t, calcOutputTo, (sample_t*, length_t, idx_t), (override));
    
    // ... required virtual methods
};

class SimpleTestComponent : public twComponent {
    // Deterministic test component (e.g., DC offset or ramp)
    // No randomness, repeatable output for validation
};

class ReverbTestComponent : public twComponent {
    // Simulated reverb: has internal state (delay line)
    // Used for testing state capture/restore
};
```

### test_helpers.h

```cpp
inline void EXPECT_AUDIO_CONTINUOUS(float prev, float next) {
    // Verify audio doesn't have discontinuities (reverb tail should fade, not jump)
    EXPECT_LT(std::abs(prev - next), 0.1f) 
        << "Audio discontinuity detected: " << prev << " -> " << next;
}

inline void EXPECT_AUDIO_EQUAL(const std::vector<float>& a, 
                                const std::vector<float>& b, 
                                float tolerance = 1e-6f) {
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i], b[i], tolerance) 
            << "Audio mismatch at sample " << i;
    }
}

inline size_t getPageCacheSize(twComponent* comp) {
    // Helper to inspect internal page cache size for testing
    return comp->getPagesInRange(0, UINT64_MAX).size();
}
```

---

## Test Execution

```bash
cd build
cmake -DBUILD_TESTS=ON ..
cmake --build .
ctest --output-on-failure
```

---

## Coverage Goals

| Category | Target | Priority |
|----------|--------|----------|
| Page cache | 95% | High |
| Freezing logic | 95% | High |
| Invalidation cascade | 90% | High |
| Thread safety | 80% | Medium |
| Integration (audio quality) | 70% | Medium |
| Error handling | 60% | Low |

---

## Future Test Extensions

When implementing Tier 3 enhancements:

1. **Pre-Freezing Predictor (#4):**
   - Test lookahead heuristics
   - Verify predictor schedules ahead correctly
   - Benchmark: no stalling in playback

2. **Benchmarking Suite (#5):**
   - Performance regression tests
   - Measure CPU before/after optimizations
   - Real-world project scenarios

3. **Component-Specific renderFrames() (#3):**
   - Test twSampleReader renderFrames() override
   - Test twGrainSource renderFrames() with input
   - Verify output matches calcOutputTo()

---

## Notes for Test Implementation

- **Google Test:** Use `FakeT est` for setup/teardown, `ASSERT_*` for hard failures, `EXPECT_*` for validation
- **Audio validation:** Use small tolerance (1e-6) for exact match, larger (0.1) for continuity
- **Threading:** Use `std::thread`, `std::atomic`, stress-test with 8+ threads
- **Mocking:** Mock components for isolation; use real components for integration tests
- **CI Integration:** Tests should run in < 30 seconds (parallelizable)

EOF
cat /tmp/test_plan_saved.md
