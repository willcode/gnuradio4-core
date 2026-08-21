#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

#include <gnuradio-4.0/ByteRing.hpp>

namespace {
/// `n` bytes counting up from `from`, so a check can name which bytes came back, not only how many.
[[nodiscard]] std::vector<std::byte> run(int from, std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0UZ; i < n; ++i) {
        v[i] = static_cast<std::byte>((from + static_cast<int>(i)) & 0xff);
    }
    return v;
}

[[nodiscard]] bool sameAs(const std::vector<std::byte>& got, int from, std::size_t n) {
    const std::vector<std::byte> want = run(from, n);
    return got.size() >= n && std::equal(want.begin(), want.end(), got.begin());
}
} // namespace

const boost::ut::suite<"gr::ByteRing"> _byteRing = [] {
    using namespace boost::ut;
    using gr::ByteRing;

    "what goes in comes out, in order"_test = [] {
        ByteRing r(1024UZ);
        expect(eq(r.capacity(), 1024UZ)) << "the capacity is what was asked for";
        expect(r.empty());

        r.push(run(0, 100UZ));
        expect(eq(r.size(), 100UZ));

        std::vector<std::byte> out(100UZ);
        expect(eq(r.pop(out.data(), 100UZ), 100UZ));
        expect(sameAs(out, 0, 100UZ)) << "byte for byte, in the order they went in";
        expect(r.empty());
    };

    "a read smaller than what is held"_test = [] {
        ByteRing r(1024UZ);
        r.push(run(0, 300UZ));

        std::vector<std::byte> out(120UZ);
        expect(eq(r.pop(out.data(), 120UZ), 120UZ));
        expect(sameAs(out, 0, 120UZ)) << "a partial pop takes from the oldest end";
        expect(eq(r.size(), 180UZ));
        expect(eq(r.pop(out.data(), 120UZ), 120UZ));
        expect(sameAs(out, 120, 120UZ)) << "the next pop continues where the first stopped";
        expect(eq(r.size(), 60UZ));
    };

    "asking for more than is held"_test = [] {
        ByteRing r(1024UZ);
        r.push(run(7, 50UZ));

        std::vector<std::byte> out(200UZ, std::byte{0xEE});
        expect(eq(r.pop(out.data(), 200UZ), 50UZ)) << "a pop past the end takes what there is and says how much";
        expect(sameAs(out, 7, 50UZ));
        expect(eq(std::to_integer<int>(out[50UZ]), 0xEE)) << "nothing was written past the bytes that were held";
        expect(r.empty());
    };

    // Nothing above reaches the end of the buffer, so nothing above exercises the two-memcpy path in either
    // direction.
    "across the end of the buffer"_test = [] {
        ByteRing r(256UZ);
        r.push(run(0, 200UZ)); // take the head most of the way along, then push across the end
        std::vector<std::byte> sink(180UZ);
        r.pop(sink.data(), 180UZ);
        expect(eq(r.size(), 20UZ)) << "twenty bytes left, near the end of the buffer";

        r.push(run(50, 100UZ));
        expect(eq(r.size(), 120UZ)) << "a push that crosses the end holds everything";

        std::vector<std::byte> out(120UZ);
        expect(eq(r.pop(out.data(), 120UZ), 120UZ)) << "and a pop that crosses it takes everything";
        expect(sameAs(out, 180, 20UZ)) << "the twenty that were already there come first";
        const std::vector<std::byte> tail(out.begin() + 20, out.end());
        expect(sameAs(tail, 50, 100UZ)) << "and the hundred that crossed the end follow them, in order";
    };

    // A sender outpacing the consumer is the case this is bounded for, and which end it drops is a behavior rather
    // than an implementation detail: the newest samples are the ones worth keeping.
    "a sender that outpaces the consumer"_test = [] {
        ByteRing r(100UZ);
        r.push(run(0, 80UZ));
        r.push(run(80, 60UZ)); // 140 into 100
        expect(eq(r.size(), 100UZ)) << "the queue holds its capacity and no more";

        std::vector<std::byte> out(100UZ);
        r.pop(out.data(), 100UZ);
        // 140 bytes of one counting run went in and the last 100 stayed, so what survives starts at 40 -- the oldest
        // 40 were dropped, not the newest.
        expect(sameAs(out, 40, 100UZ)) << "and what survived is the newest hundred bytes";
    };

    "one push larger than the whole queue"_test = [] {
        ByteRing r(100UZ);
        r.push(run(0, 250UZ));
        expect(eq(r.size(), 100UZ)) << "a push bigger than the capacity fills it";

        std::vector<std::byte> out(100UZ);
        r.pop(out.data(), 100UZ);
        expect(sameAs(out, 150, 100UZ)) << "with the tail of what was pushed, which is the newest part";
    };

    "the degenerate shapes"_test = [] {
        std::vector<std::byte> out(4UZ);

        ByteRing zero(0UZ);
        zero.push(run(0, 10UZ));
        expect(zero.empty()) << "a queue of no capacity holds nothing";
        expect(eq(zero.size(), 0UZ));
        expect(eq(zero.pop(out.data(), 4UZ), 0UZ)) << "and gives nothing back";

        ByteRing r(64UZ);
        r.push({});
        expect(r.empty()) << "an empty push holds nothing";
        expect(eq(r.pop(out.data(), 0UZ), 0UZ)) << "and a pop of nothing takes nothing";

        r.push(run(0, 40UZ));
        r.clear();
        expect(r.empty()) << "clear empties it";
        expect(eq(r.capacity(), 64UZ)) << "and leaves the capacity where it was";

        r.push(run(0, 40UZ));
        r.dropOldest(1000UZ);
        expect(r.empty()) << "dropping more than is held empties it rather than underflowing";
    };

    // peek must leave the bytes in place: a caller uses it to check it can commit before consuming.
    "peek does not consume"_test = [] {
        ByteRing r(256UZ);
        r.push(run(11, 90UZ));

        std::vector<std::byte> a(90UZ);
        std::vector<std::byte> b(90UZ);
        expect(eq(r.peek(a.data(), 90UZ), 90UZ)) << "a peek reads what is held";
        expect(eq(r.size(), 90UZ)) << "and leaves it there";
        expect(eq(r.peek(b.data(), 90UZ), 90UZ)) << "so a second peek reads the same bytes";
        expect(std::ranges::equal(a, b));
        expect(sameAs(a, 11, 90UZ));
        expect(eq(r.pop(a.data(), 90UZ), 90UZ));
        expect(r.empty());
    };
};

int main() { /* not needed for UT */ }
