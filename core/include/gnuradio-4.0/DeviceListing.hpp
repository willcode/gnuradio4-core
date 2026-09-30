#ifndef GNURADIO_DEVICE_LISTING_HPP
#define GNURADIO_DEVICE_LISTING_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Export.hpp>

namespace gr {

/// One attached unit as its family's listing reports it.
struct DeviceUnit {
    std::string label;    ///< a name for a person, empty where the family gives none
    std::string selector; ///< the string that names this unit to the family's blocks, empty where the family has none
};

/**
 * @brief The listing a device family registers beside its blocks.
 *
 * A listing holds the family word, three flags and the function that lists the attached units. Core carries `family`
 * and each unit's `selector` as strings and reads neither. `opens` states whether `listUnits` opens a unit to find it.
 * `receive` and `transmit` state the directions of the family's blocks, not those of a unit's hardware. `listUnits`
 * may throw. A device listing then reports the family as not listed, with the exception's message.
 */
struct DeviceListing {
    using ListFunction = std::vector<DeviceUnit> (*)();

    std::string  family;
    bool         opens     = false;
    bool         receive   = false;
    bool         transmit  = false;
    ListFunction listUnits = nullptr;
};

/**
 * @brief The device listings of a program or of one plugin, at most one per family word.
 *
 * A program and its block libraries register into `globalDeviceListingRegistry()`. A plugin registers into its own
 * instance, which the plugin interface returns from `deviceListings()`. The first registration of a family word holds
 * it. The same function offered again under that word adds nothing. Another function under that word is refused and
 * recorded in `refused()`. A listing states no version, and the family word alone is the key. A listing version would
 * join the word in that key, as a block's version joins its name in `detail::claimVersion()`.
 */
class DeviceListingRegistry {
public:
    /// A registration refused because an earlier registration holds its family word.
    struct Refusal {
        std::string                 family;
        DeviceListing::ListFunction refused = nullptr; ///< the function the refused registration offered
        DeviceListing::ListFunction holder  = nullptr; ///< the function of the registration that holds the word
    };

private:
    std::vector<DeviceListing> _listings;
    std::vector<Refusal>       _refused;

public:
    /// files `listing` under its family word unless a registration holds that word. Returns whether it was added.
    bool add(DeviceListing listing) {
        const auto held = std::ranges::find(_listings, listing.family, &DeviceListing::family);
        if (held == _listings.end()) {
            _listings.push_back(std::move(listing));
            return true;
        }
        if (held->listUnits != listing.listUnits) {
            _refused.push_back({.family = held->family, .refused = listing.listUnits, .holder = held->listUnits});
        }
        return false;
    }

    /// the listings held, in the order they were registered
    [[nodiscard]] const std::vector<DeviceListing>& listings() const noexcept { return _listings; }

    /// the number of family words held. A registration that adds a word raises it by one. An erase lowers it.
    [[nodiscard]] std::size_t generation() const noexcept { return _listings.size(); }

    /// removes the listing that holds `family`, before the file whose code the listing points into is closed. The
    /// recorded refusals stay.
    void erase(std::string_view family) { std::erase_if(_listings, [family](const DeviceListing& listing) { return listing.family == family; }); }

    /// every registration this registry refused, in the order they were offered
    [[nodiscard]] const std::vector<Refusal>& refused() const noexcept { return _refused; }
};

/// The registry a program and the block libraries it loads register their device listings into.
GNURADIO_EXPORT
DeviceListingRegistry& globalDeviceListingRegistry();

/// Whether a device listing may ask a family whose listing opens a unit.
enum class Opening : std::uint8_t {
    Avoided, ///< such a family is left out, and reported with the reason
    Allowed, ///< every family is asked
};

/// One unit a device listing found, with the word and the directions of its family.
struct ListedUnit {
    std::string family;
    std::string label;
    std::string selector;
    bool        receive  = false;
    bool        transmit = false;
};

/// What one device listing did with one family.
struct ListedFamily {
    std::string family;
    bool        opens  = false; ///< the flag the family states
    bool        listed = false; ///< whether the listing asked the family and the family answered
    std::string reason;         ///< why the family reported no unit, empty where it reported one
};

/// The units a device listing found, and every family it asked or left out, both ordered by family word.
struct DeviceList {
    std::vector<ListedUnit>   units;
    std::vector<ListedFamily> families;
};

namespace detail {

/// the units of each of `listings` that `opening` lets the call ask, and a report of every one of them
[[nodiscard]] inline DeviceList listDevices(std::vector<DeviceListing> listings, Opening opening) {
    std::ranges::stable_sort(listings, {}, &DeviceListing::family);
    DeviceList list;
    for (const DeviceListing& listing : listings) {
        ListedFamily& family = list.families.emplace_back(ListedFamily{.family = listing.family, .opens = listing.opens, .listed = false, .reason = {}});
        if (listing.opens && opening == Opening::Avoided) {
            family.reason = "its listing opens a unit, and the call does not allow that";
            continue;
        }
        if (listing.listUnits == nullptr) {
            family.reason = "the family registered no listing function";
            continue;
        }
        std::vector<DeviceUnit> units;
        try {
            units = listing.listUnits();
        } catch (const std::exception& error) {
            family.reason = error.what();
            continue;
        } catch (...) {
            family.reason = "the listing failed with an exception that carries no message";
            continue;
        }
        family.listed = true;
        if (units.empty()) {
            family.reason = "the listing found no unit";
        }
        for (DeviceUnit& unit : units) {
            list.units.push_back({.family = listing.family, .label = std::move(unit.label), .selector = std::move(unit.selector), .receive = listing.receive, .transmit = listing.transmit});
        }
    }
    return list;
}

} // namespace detail

} // namespace gr

#endif // GNURADIO_DEVICE_LISTING_HPP
