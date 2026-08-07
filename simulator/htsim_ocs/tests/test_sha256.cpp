#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include "sha256.h"

int main() {
    const std::vector<std::pair<std::string_view, std::string_view>> vectors = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (const auto& [input, expected] : vectors) {
        const std::string actual = htsim_ocs::sha256_hex(input);
        if (actual != expected) {
            std::cerr << "SHA-256 vector mismatch: " << actual << '\n';
            return 1;
        }
    }
    std::cout << "sha256 NIST vectors: PASS\n";
    return 0;
}
