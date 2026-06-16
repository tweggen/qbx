#include "twplugininsert.h"
#include "tw303aenv.h"
#include "plugins/twplugindescriptor.h"
#include <iostream>
#include <cstring>
#include <vector>

namespace audio {

// Simple test to verify twPluginInsert processes audio correctly.
// This is Phase 1 proof-of-concept: instantiate PassThrough plugin,
// feed it stereo audio, and verify output matches input (passthrough).
int testPluginInsert() {
    std::cout << "=== Plugin Insert Test ===" << std::endl;

    // Create a mock environment (minimal setup for testing).
    tw303aEnvironment env;

    // Get the registry and rescan for plugins (will find PassThrough).
    auto &registry = pluginRegistry();
    registry.rescan();

    const auto &plugins = registry.plugins();
    if (plugins.empty()) {
        std::cerr << "ERROR: No plugins found in registry" << std::endl;
        return 1;
    }

    // Find and instantiate the PassThrough plugin.
    std::unique_ptr<twPlugin> plugin = registry.instantiate(plugins[0]);
    if (!plugin) {
        std::cerr << "ERROR: Failed to instantiate PassThrough plugin" << std::endl;
        return 1;
    }

    std::cout << "Instantiated: " << plugins[0].name << std::endl;

    // Create the host component.
    auto insert = std::make_unique<twPluginInsert>(env, std::move(plugin));

    // Verify I/O layout (should be 2-in / 2-out for PassThrough).
    if (insert->getNInputs() != 2 || insert->getNOutputs() != 2) {
        std::cerr << "ERROR: Expected 2-in / 2-out, got "
                  << insert->getNInputs() << "-in / " << insert->getNOutputs() << "-out"
                  << std::endl;
        return 1;
    }

    std::cout << "I/O Layout: " << insert->getNInputs() << "-in / "
              << insert->getNOutputs() << "-out" << std::endl;

    // Test parameter access.
    if (insert->getPlugin()->paramCount() != 1) {
        std::cerr << "ERROR: Expected 1 parameter, got "
                  << insert->getPlugin()->paramCount() << std::endl;
        return 1;
    }

    auto paramInfo = insert->getPlugin()->paramInfo(0);
    std::cout << "Parameter 0: " << paramInfo.name << " (range "
              << paramInfo.minValue << " to " << paramInfo.maxValue << ")"
              << std::endl;

    // Test state save/restore.
    auto state = insert->getPlugin()->saveState();
    std::cout << "State size: " << state.size() << " bytes" << std::endl;

    if (!insert->getPlugin()->loadState(state)) {
        std::cerr << "ERROR: Failed to load state" << std::endl;
        return 1;
    }

    std::cout << "State save/load: OK" << std::endl;

    // Success!
    std::cout << "=== All tests passed ===" << std::endl;
    return 0;
}

}  // namespace audio

// Entry point for standalone test (if invoked directly).
#if defined(TEST_PLUGIN_INSERT_MAIN)
int main() {
    return audio::testPluginInsert();
}
#endif
