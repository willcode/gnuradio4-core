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

    "an empty installation directory adds no directory, and the named directories are searched alone"_test = [] {
        expect(fatal(::setenv("GNURADIO4_PLUGIN_DIRECTORIES", "/environment/only", 1) == 0));
        const std::vector<std::string> fromCommandLine{"/command/line"};

        const std::vector<gr::tools::Directory> withInstallation = gr::tools::searchDirectories(fromCommandLine, "/installation");
        expect(fatal(eq(withInstallation.size(), 3UZ))) << "the instrument: a named installation directory is searched";
        expect(eq(withInstallation.back().origin, std::string("installation")));

        const std::vector<gr::tools::Directory> alone = gr::tools::searchDirectories(fromCommandLine, "");
        expect(fatal(eq(alone.size(), 2UZ)));
        expect(eq(alone[0].origin, std::string("option")));
        expect(eq(alone[1].origin, std::string("environment")));
    };

    "a declared role the stream ports contradict yields a note, and one they agree with yields none"_test = [] {
        expect(eq(gr::tools::roleNote("source", "sink").value_or(""), std::string("role/source declared; the stream ports read sink")));
        expect(gr::tools::roleNote("sink", "source").has_value());
        expect(eq(gr::tools::roleNote("processor", "").value_or(""), std::string("role/processor declared; the stream ports read no role")));
        expect(gr::tools::roleNote("notation", "source").has_value());
        expect(!gr::tools::roleNote("source", "source").has_value());
        expect(!gr::tools::roleNote("source", "processor").has_value()) << "a device source may take a stream input";
        expect(!gr::tools::roleNote("sink", "").has_value());
        expect(!gr::tools::roleNote("transceiver", "processor").has_value());
        expect(!gr::tools::roleNote("", "sink").has_value()) << "nothing declared, nothing contradicted";
    };

    "the role line carries the declared word, else the word the stream ports read"_test = [] {
        expect(eq(gr::tools::roleText("source", "sink"), std::string("source, declared")));
        expect(eq(gr::tools::roleText("", "processor"), std::string("processor, read from the stream ports")));
        expect(eq(gr::tools::roleText("", ""), std::string("none, read from the stream ports")));
    };
};

int main() { /* not needed for UT */ }
