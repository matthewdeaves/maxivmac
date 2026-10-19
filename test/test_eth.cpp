// Tests for the EthernetBackend seam: FrameQueue and FakeEthernet.
#include <doctest/doctest.h>

#include "eth_fake.h"

TEST_CASE("eth queue: empty queue has no frame")
{
	FakeEthernet e;
	CHECK(e.peekSize() == 0);
	uint8_t buf[kEthMaxFrame];
	CHECK(e.receive(buf) == 0);
}

TEST_CASE("eth queue: peekSize does not dequeue, receive does, FIFO order")
{
	FakeEthernet e;
	REQUIRE(e.inject(makeFrame(60, 1)));
	REQUIRE(e.inject(makeFrame(100, 2)));
	CHECK(e.peekSize() == 60);
	CHECK(e.peekSize() == 60);
	uint8_t buf[kEthMaxFrame];
	CHECK(e.receive(buf) == 60);
	CHECK(buf[20] == 1);
	CHECK(e.peekSize() == 100);
	CHECK(e.receive(buf) == 100);
	CHECK(buf[20] == 2);
	CHECK(e.peekSize() == 0);
}

TEST_CASE("eth queue: receive into a too-small buffer leaves the frame queued")
{
	FakeEthernet e;
	e.inject(makeFrame(100, 7));
	uint8_t small[50];
	CHECK(e.receive(small) == 0);
	CHECK(e.peekSize() == 100);
}

TEST_CASE("eth queue: bounded at 64 frames, oldest dropped and counted")
{
	FakeEthernet e;
	for (int i = 0; i < 70; i++) e.inject(makeFrame(60, uint8_t(i)));
	CHECK(e.queued() == kEthRxQueueFrames);
	CHECK(e.dropped() == 6);
	uint8_t buf[kEthMaxFrame];
	REQUIRE(e.receive(buf) == 60);
	CHECK(buf[20] == 6); // frames 0..5 were dropped
}

TEST_CASE("eth queue: out-of-range frame lengths are rejected")
{
	FakeEthernet e;
	CHECK_FALSE(e.inject(makeFrame(13, 0)));
	CHECK_FALSE(e.inject(makeFrame(1515, 0)));
	CHECK(e.inject(makeFrame(14, 0)));
	CHECK(e.inject(makeFrame(1514, 0)));
	CHECK(e.dropped() == 0);
}

TEST_CASE("eth fake: send validates length and link, records frames")
{
	FakeEthernet e;
	CHECK(e.send(makeFrame(60, 3)));
	CHECK_FALSE(e.send(makeFrame(13, 3)));
	CHECK_FALSE(e.send(makeFrame(1515, 3)));
	e.link = false;
	CHECK_FALSE(e.send(makeFrame(60, 3)));
	CHECK(e.sent.size() == 1);
}

TEST_CASE("eth fake: link down hides received frames, link up restores them")
{
	FakeEthernet e;
	e.inject(makeFrame(60, 1));
	e.link = false;
	CHECK(e.peekSize() == 0);
	e.link = true;
	CHECK(e.peekSize() == 60);
}
