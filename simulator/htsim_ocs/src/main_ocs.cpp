#include <iostream>
#include <string_view>

#include "version.h"

namespace {

void print_help(std::ostream& output) {
    output << "Usage: htsim_ocs <command>\n"
           << "\n"
           << "Commands:\n"
           << "  help       Show this help message\n"
           << "  version    Show build and contract versions\n";
}

void print_version(std::ostream& output) {
    output << "htsim_ocs program version: " << htsim_ocs::version::kProgram
           << '\n'
           << "execution plan schema version: "
           << htsim_ocs::version::kExecutionPlanSchema << '\n'
           << "result schema version: " << htsim_ocs::version::kResultSchema
           << '\n'
           << "csg-htsim upstream commit: "
           << htsim_ocs::version::kHtsimUpstreamCommit << '\n'
           << "local patchset identifier: "
           << htsim_ocs::version::kLocalPatchsetSha256 << '\n'
           << "parent repository build commit: "
           << htsim_ocs::version::kParentBuildCommit << '\n'
           << "parent repository build state: "
           << htsim_ocs::version::kParentBuildState << '\n'
           << "compiler: " << __VERSION__ << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "htsim_ocs: missing command\n"
                  << "Try 'htsim_ocs help'.\n";
        return 2;
    }

    if (argc > 2) {
        std::cerr << "htsim_ocs: unexpected argument: " << argv[2] << '\n';
        return 2;
    }

    const std::string_view command(argv[1]);
    if (command == "help") {
        print_help(std::cout);
        return 0;
    }
    if (command == "version") {
        print_version(std::cout);
        return 0;
    }

    std::cerr << "htsim_ocs: unknown command: " << command << '\n'
              << "Try 'htsim_ocs help'.\n";
    return 2;
}
