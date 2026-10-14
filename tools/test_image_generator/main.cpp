// test_image_generator: writes the test corpus (tests/support/corpus) as files,
// for development and testing only. Every scenario is reproducible: the same
// image, byte for byte, each time.
//
//   test_image_generator list
//   test_image_generator manifest NAME
//   test_image_generator write --output FOLDER [--originals] [NAME ...]
//   test_image_generator fuzz-seeds --output FOLDER
//
// `write` makes <name>.img, its metadata (<name>.img.imgmeta) when the image
// has unreadable regions or sectors other than 512 bytes, <name>.manifest.txt
// (what the image holds and what recovery must give back of each file), and
// with --originals the original files (<name>.originals/<sha256 prefix>.bin).
// It never overwrites a file. Without names it writes every scenario.
//
// `fuzz-seeds` writes seed inputs for every fuzz target (fuzz/) below
// FOLDER/<target>/.

#include "fuzz_seeds.hpp"
#include "recovery/text.hpp"
#include "support/corpus.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace recovery::test;

int usage() {
    std::cerr << "usage: test_image_generator list\n"
                 "       test_image_generator manifest NAME\n"
                 "       test_image_generator write --output FOLDER [--originals] [NAME ...]\n"
                 "       test_image_generator fuzz-seeds --output FOLDER\n";
    return 1;
}

int list() {
    for (const corpus::Scenario& scenario : corpus::scenarios()) {
        std::cout << scenario.name << "\n    " << scenario.summary << "\n    categories:";
        for (std::size_t i = 0; i < scenario.categories.size(); ++i) {
            std::cout << (i == 0 ? " " : ", ") << scenario.categories[i];
        }
        std::cout << "\n";
    }
    return 0;
}

int manifest(const std::string& name) {
    const corpus::Scenario* scenario = corpus::findScenario(name);
    if (scenario == nullptr) {
        std::cerr << "no scenario named " << name << " (test_image_generator list)\n";
        return 1;
    }
    std::cout << corpus::manifest(*scenario, corpus::generate(*scenario));
    return 0;
}

int write(const std::vector<std::string>& arguments) {
    std::filesystem::path output;
    bool originals = false;
    std::vector<const corpus::Scenario*> chosen;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "--output" && i + 1 < arguments.size()) {
            std::u8string text;
            for (const char c : arguments[++i]) {
                text.push_back(static_cast<char8_t>(c));
            }
            output = std::filesystem::path(text);
        } else if (arguments[i] == "--originals") {
            originals = true;
        } else if (const corpus::Scenario* scenario = corpus::findScenario(arguments[i])) {
            chosen.push_back(scenario);
        } else {
            std::cerr << "no scenario named " << arguments[i] << " (test_image_generator list)\n";
            return 1;
        }
    }
    if (output.empty()) {
        return usage();
    }
    if (chosen.empty()) {
        for (const corpus::Scenario& scenario : corpus::scenarios()) {
            chosen.push_back(&scenario);
        }
    }
    for (const corpus::Scenario* scenario : chosen) {
        const corpus::Image image = corpus::generate(*scenario);
        if (recovery::Status written = corpus::write(output, *scenario, image, originals); !written.ok()) {
            std::cerr << scenario->name << ": " << recovery::describe(written.error()) << "\n";
            return 2;
        }
        std::cout << scenario->name << ": " << image.bytes.size() << " bytes, " << image.originals.size()
                  << " original files\n";
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.push_back(recovery::toUtf8(std::filesystem::path(argv[i])));
        }
        if (arguments.empty()) {
            return usage();
        }
        const std::string command = arguments.front();
        arguments.erase(arguments.begin());
        if (command == "list" && arguments.empty()) {
            return list();
        }
        if (command == "manifest" && arguments.size() == 1) {
            return manifest(arguments.front());
        }
        if (command == "write") {
            return write(arguments);
        }
        if (command == "fuzz-seeds" && arguments.size() == 2 && arguments[0] == "--output") {
            std::u8string folder;
            for (const char c : arguments[1]) {
                folder.push_back(static_cast<char8_t>(c));
            }
            if (const recovery::Status written = corpus::writeFuzzSeeds(std::filesystem::path(folder)); !written.ok()) {
                std::cerr << "fuzz-seeds: " << recovery::describe(written.error()) << "\n";
                return 2;
            }
            std::cout << "fuzz seeds written\n";
            return 0;
        }
        return usage();
    } catch (const std::exception& error) {
        std::cerr << "test_image_generator: " << error.what() << "\n";
        return 2;
    }
}
