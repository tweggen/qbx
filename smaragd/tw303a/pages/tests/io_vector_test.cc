#include "tw/pages/io_vector.h"
#include <iostream>

int main() {
    std::cout << "IOVector tests compiled successfully\n";
    
    // Basic test: create and validate
    auto page = std::make_shared<twOutputPage>();
    page->samples.resize(twOutputPage::FRAME_CAPACITY, 1.0f);
    page->validFrames = twOutputPage::FRAME_CAPACITY;
    
    IOVector vec(page, 0, 1000);
    if (!vec.validate()) {
        std::cerr << "FAILED: vec.validate()\n";
        return 1;
    }
    
    std::cout << "✓ Basic validation passed\n";
    
    // Test copy
    auto dstPage = std::make_shared<twOutputPage>();
    dstPage->samples.resize(twOutputPage::FRAME_CAPACITY, 0.0f);
    
    IOVector dst(dstPage, 0, 1000);
    length_t copied = dst.copyFrom(vec, 0, 100);
    if (copied != 100) {
        std::cerr << "FAILED: copyFrom returned " << copied << ", expected 100\n";
        return 1;
    }
    
    std::cout << "✓ Copy test passed\n";
    
    // Test mix
    length_t mixed = dst.mixFrom(vec, 0, 50);
    if (mixed != 50) {
        std::cerr << "FAILED: mixFrom returned " << mixed << ", expected 50\n";
        return 1;
    }
    
    std::cout << "✓ Mix test passed\n";
    std::cout << "\nAll basic tests passed!\n";
    
    return 0;
}
