#include <boost/ut.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include <gnuradio-4.0/BlockLookup.hpp>

// The tools' own headers sit beside their sources, and this program is built without that directory on its include
// path. The lookup below reaches it through core's include directory, which the installation copies.
#if __has_include("RegistryDoc.hpp") || __has_include("GraphDoc.hpp")
#error "the tools' source directory is on the include path of the lookup's consumer"
#endif

/**
 * A program outside the tools that includes the block lookup by its public path and uses it.
 */
const boost::ut::suite<"BlockLookupConsumer"> blockLookupConsumerTests = [] {
    using namespace boost::ut;

    "the search order is the command line, then the environment, then the installation, each directory once"_test = [] {
        expect(fatal(::setenv("GNURADIO4_PLUGIN_DIRECTORIES", "/environment/first:/command/line:/environment/second", 1) == 0));
        const std::vector<std::string>          fromCommandLine{"/command/line", "/command/line"};
        const std::vector<gr::tools::Directory> directories = gr::tools::searchDirectories(fromCommandLine, "/environment/second");
        const std::vector<std::string>          expectedPaths{"/command/line", "/environment/first", "/environment/second"};
        const std::vector<std::string>          expectedOrigins{"option", "environment", "environment"};
        expect(fatal(eq(directories.size(), expectedPaths.size())));
        for (std::size_t i = 0UZ; i < directories.size(); ++i) {
            expect(eq(directories[i].path, expectedPaths[i]));
            expect(eq(directories[i].origin, expectedOrigins[i]));
            expect(!directories[i].present) << "a directory that does not exist reads as present";
        }
    };

    "a declared role the stream ports contradict yields a note, and one they agree with yields none"_test = [] {
        expect(gr::tools::roleNote("source", "consumer").has_value());
        expect(!gr::tools::roleNote("source", "generator").has_value());
    };
};

int main() { /* not needed for UT */ }
