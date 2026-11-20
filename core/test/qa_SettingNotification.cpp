#include <boost/ut.hpp>

#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <gnuradio-4.0/SettingNotification.hpp>

namespace qa_setting_notification {

using namespace boost::ut;
using gr::settings::Notification;
using gr::settings::NotificationKind;
using gr::settings::NotificationLevel;
using gr::settings::detail::NotificationQueue;

const suite<"SettingNotification"> tests = [] {
    "each level and kind has its name"_test = [] {
        for (NotificationLevel level : {NotificationLevel::trace, NotificationLevel::debug, NotificationLevel::info, NotificationLevel::warning, NotificationLevel::error, NotificationLevel::critical, NotificationLevel::off}) {
            expect(gr::settings::parseNotificationLevel(gr::settings::notificationLevelName(level)) == level);
        }
        expect(!gr::settings::parseNotificationLevel("loud").has_value());
        expect(!gr::settings::parseNotificationLevel("").has_value());
        expect(gr::settings::notificationKindName(NotificationKind::changed) == "changed");
        expect(gr::settings::notificationKindName(NotificationKind::failed) == "failed");
    };

    "the queue keeps the latest per block, key and level with the count it replaced"_test = [] {
        NotificationQueue queue;
        for (const char* reason : {"out of range", "out of range", "device lost"}) {
            queue.push(Notification{.level = NotificationLevel::error, .what = NotificationKind::failed, .block = "src", .key = "gain", .reason = reason});
        }
        queue.push(Notification{.level = NotificationLevel::info, .what = NotificationKind::changed, .block = "src", .key = "gain"});
        queue.push(Notification{.level = NotificationLevel::error, .what = NotificationKind::failed, .block = "sink", .key = "gain", .reason = "full"});
        const std::vector<Notification> held = queue.take();
        expect(fatal(eq(held.size(), 3UZ)));
        expect(held[0].level == NotificationLevel::error && held[0].block == "src" && held[0].reason == "device lost");
        expect(eq(held[0].nReplaced, 2UZ));
        expect(held[1].level == NotificationLevel::info && eq(held[1].nReplaced, 0UZ));
        expect(held[2].block == "sink");
        expect(queue.take().empty());
    };

    "a replaced notification keeps the count of those it replaced"_test = [] {
        NotificationQueue queue;
        queue.push(Notification{.block = "src", .key = "gain", .nReplaced = 4UZ});
        queue.push(Notification{.block = "src", .key = "gain"});
        const std::vector<Notification> taken = queue.take();
        expect(fatal(eq(taken.size(), 1UZ)));
        expect(eq(taken.front().nReplaced, 5UZ));
    };

    "the default reader prints one line naming the block, the key and the reason"_test = [] {
        std::ostringstream captured;
        std::streambuf*    previous = std::cerr.rdbuf(captured.rdbuf());
        gr::settings::detail::printNotification(Notification{.level = NotificationLevel::error, .what = NotificationKind::failed, .block = "src#1", .key = "gain", .reason = "no lock"});
        gr::settings::detail::printNotification(Notification{.level = NotificationLevel::info, .what = NotificationKind::changed, .block = "src#1", .key = {}, .reason = {}});
        std::cerr.rdbuf(previous);
        expect(eq(captured.str(), std::string("error: src#1 failed 'gain': no lock\ninfo: src#1 changed\n")));
    };
};

} // namespace qa_setting_notification

int main() { /* not needed by the UT framework */ }
