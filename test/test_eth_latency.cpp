// EthLatency: received frames are held back on the emulated clock.
#include <doctest/doctest.h>

#include "devices/eth_latency.h"
#include "devices/scsi_daynaport.h"
#include "devices/scsi_factory.h"
#include "devices/eth_spec.h"
#include "eth_fake.h"

namespace
{
// A backend that answers every sent frame at once, as libslirp does.
class Echo : public FakeEthernet
{
public:
	bool send(std::span<const uint8_t> f) override
	{
		const bool ok = FakeEthernet::send(f);
		if (ok) inject(makeFrame(f.size(), 0xEE));
		return ok;
	}
};

struct Rig
{
	uint64_t now = 1000; // fake emulated clock, arbitrary start
	Echo *fake = nullptr;
	std::unique_ptr<EthLatency> lat;
	explicit Rig(uint64_t latency)
	{
		auto e = std::make_unique<Echo>();
		fake = e.get();
		lat = std::make_unique<EthLatency>(std::move(e), [this] { return now; }, latency);
	}
};
} // namespace

TEST_CASE("eth latency: a frame answered inside send() is not visible until the latency has passed")
{
	Rig r(100);
	REQUIRE(r.lat->send(makeFrame(60, 1)));
	CHECK(r.lat->peekSize() == 0);
	std::vector<uint8_t> buf(kEthMaxFrame);
	CHECK(r.lat->receive(buf) == 0);
	r.now += 99;
	CHECK(r.lat->peekSize() == 0);
	r.now += 1; // exactly `latency` after the send
	CHECK(r.lat->peekSize() == 60);
	CHECK(r.lat->receive(buf) == 60);
	CHECK(buf[20] == 0xEE);
	CHECK(r.lat->peekSize() == 0);
}

TEST_CASE("eth latency: zero latency passes frames straight through")
{
	Rig r(0);
	REQUIRE(r.lat->send(makeFrame(70, 1)));
	CHECK(r.lat->peekSize() == 70);
}

TEST_CASE("eth latency: peeking never consumes and order is preserved")
{
	Rig r(50);
	r.fake->inject(makeFrame(60, 1));
	r.lat->poll(); // stamped at t=1000
	r.now += 30;
	r.fake->inject(makeFrame(80, 2));
	r.lat->poll(); // stamped at t=1030
	r.now += 20;   // t=1050: first is due, second is not
	CHECK(r.lat->peekSize() == 60);
	CHECK(r.lat->peekSize() == 60);
	std::vector<uint8_t> buf(kEthMaxFrame);
	CHECK(r.lat->receive(buf) == 60);
	CHECK(r.lat->peekSize() == 0);
	r.now += 30;
	CHECK(r.lat->peekSize() == 80);
}

TEST_CASE("eth latency: a frame becomes visible by the clock, not by the number of polls")
{
	Rig r(100);
	r.fake->inject(makeFrame(60, 1));
	for (int i = 0; i < 1000; i++) r.lat->poll();
	CHECK(r.lat->peekSize() == 0);
	r.now += 100;
	CHECK(r.lat->peekSize() == 60);
}

TEST_CASE("eth latency: receive needs a big enough buffer and keeps the frame otherwise")
{
	Rig r(0);
	r.fake->inject(makeFrame(100, 1));
	std::vector<uint8_t> small(50);
	CHECK(r.lat->receive(small) == 0);
	CHECK(r.lat->peekSize() == 100);
}

TEST_CASE("eth latency: link state, name and send result come from the inner backend")
{
	Rig r(10);
	CHECK(std::string(r.lat->name()) == "fake");
	r.fake->link = false;
	CHECK_FALSE(r.lat->linkUp());
	CHECK_FALSE(r.lat->send(makeFrame(60, 1)));
	r.fake->inject(makeFrame(60, 1));
	r.now += 100;
	CHECK(r.lat->peekSize() == 0); // cable out: nothing is delivered
	r.fake->link = true;
	CHECK(r.lat->peekSize() == 0); // the frame the fake held while down is still in it
}

TEST_CASE("eth latency: held queue is bounded, drop-oldest, and counted with the inner drops")
{
	Rig r(1'000'000);
	for (size_t i = 0; i < kEthRxQueueFrames + 5; i++)
	{
		r.fake->inject(makeFrame(60, uint8_t(i)));
		r.lat->poll();
	}
	CHECK(r.lat->held() == kEthRxQueueFrames);
	CHECK(r.lat->droppedFrames() == 5);
	r.now += 1'000'000;
	std::vector<uint8_t> buf(kEthMaxFrame);
	REQUIRE(r.lat->receive(buf) == 60);
	CHECK(buf[20] == 5); // frames 0..4 were the oldest
}

TEST_CASE("eth latency: the DaynaPORT READ after a WRITE is empty until the latency has passed")
{
	Rig r(100);
	DaynaPortTarget t(std::move(r.lat));
	const uint8_t enable[6] = {0x0E, 0, 0, 0, 0, 0x80};
	t.command(0, enable);
	const uint8_t write[6] = {0x0A, 0, 0, 0, 60, 0};
	scsi::Step s = t.command(0, write);
	REQUIRE(s.kind == scsi::Step::DataOut);
	const auto frame = makeFrame(60, 7);
	std::copy(frame.begin(), frame.end(), s.out.begin());
	t.dataOutDone();

	const uint8_t read[6] = {0x08, 0, 0, 0x05, 0xF4, 0xC0};
	s = t.command(0, read);
	REQUIRE(s.kind == scsi::Step::DataIn);
	CHECK(s.in.size() == 6); // empty record: the echo is still "on the wire"
	t.dataInDone();
	r.now += 100;
	s = t.command(0, read);
	CHECK(s.in.size() == 6 + 60 + 4);
	CHECK(t.stats().rxFrames == 1);
}

TEST_CASE("eth latency: --eth-latency durations")
{
	const uint64_t hz = 7'833'600ull * 8;
	uint64_t v = 99;
	std::string err;
	REQUIRE(parseEthLatency("2ms", hz, v, err));
	CHECK(v == hz * 2 / 1000);
	REQUIRE(parseEthLatency("500us", hz, v, err));
	CHECK(v == hz * 500 / 1'000'000);
	REQUIRE(parseEthLatency("1s", hz, v, err));
	CHECK(v == hz);
	for (const char *off : {"0", "off", "none"})
	{
		v = 5;
		REQUIRE(parseEthLatency(off, hz, v, err));
		CHECK(v == 0);
	}
	for (const char *bad : {"", "2", "ms", "-1ms", "2 ms", "2c", "2k", "1.5ms", "2h", "2s5", "2001ms", "2s1", "99999999s"})
		CHECK_MESSAGE(!parseEthLatency(bad, hz, v, err), bad);
	CHECK(err.find("--eth-latency") != std::string::npos);
}

TEST_CASE("eth latency: the scsi factory wraps the backend (default 2 ms) only when given a clock")
{
	ScsiTargetOptions o;
	o.eth = "none";
	auto plain = CreateScsiTarget("daynaport", 4, o);
	CHECK(dynamic_cast<EthLatency *>(&dynamic_cast<DaynaPortTarget *>(plain.get())->backend()) == nullptr);

	uint64_t now = 0;
	o.clock = [&] { return now; };
	o.clockUnitsPerSecond = 1000;
	auto t = CreateScsiTarget("daynaport", 4, o);
	auto *lat = dynamic_cast<EthLatency *>(&dynamic_cast<DaynaPortTarget *>(t.get())->backend());
	REQUIRE(lat);
	CHECK(lat->latency() == 2); // 2 ms at 1000 units/s

	o.ethLatency = "0";
	auto off = CreateScsiTarget("daynaport", 4, o);
	CHECK(dynamic_cast<EthLatency *>(&dynamic_cast<DaynaPortTarget *>(off.get())->backend()) == nullptr);

	o.ethLatency = "10ms";
	auto ten = CreateScsiTarget("daynaport", 4, o);
	CHECK(dynamic_cast<EthLatency *>(&dynamic_cast<DaynaPortTarget *>(ten.get())->backend())->latency() == 10);

	o.ethLatency = "bogus"; // warns and falls back to the default
	auto bad = CreateScsiTarget("daynaport", 4, o);
	CHECK(dynamic_cast<EthLatency *>(&dynamic_cast<DaynaPortTarget *>(bad.get())->backend())->latency() == 2);
}
