// Tests for CreateScsiTarget: type/option parsing and the --eth default rule.
#include <doctest/doctest.h>

#include "devices/scsi_daynaport.h"
#include "devices/scsi_factory.h"

namespace
{
DaynaPortTarget *asDayna(const std::unique_ptr<scsi::ScsiTarget> &t)
{
	return dynamic_cast<DaynaPortTarget *>(t.get());
}
} // namespace

TEST_CASE("scsi factory: none and unknown types give no target")
{
	ScsiTargetOptions o;
	CHECK(CreateScsiTarget("", 4, o) == nullptr);
	CHECK(CreateScsiTarget("none", 4, o) == nullptr);
	CHECK(CreateScsiTarget("floppy", 4, o) == nullptr);
}

TEST_CASE("scsi factory: daynaport packs one frame per READ unless 'multiframe' is given")
{
	ScsiTargetOptions o;
	o.eth = "none";
	auto a = CreateScsiTarget("daynaport", 4, o);
	REQUIRE(asDayna(a));
	CHECK_FALSE(asDayna(a)->options().multiFrame);
	auto b = CreateScsiTarget("daynaport,multiframe", 4, o);
	REQUIRE(asDayna(b));
	CHECK(asDayna(b)->options().multiFrame);
	auto c = CreateScsiTarget("daynaport,bogus", 4, o); // warns, still created
	REQUIRE(asDayna(c));
	CHECK_FALSE(asDayna(c)->options().multiFrame);
}

TEST_CASE("scsi factory: an unattended run without --eth gets no network, an explicit --eth wins")
{
	ScsiTargetOptions o;
	o.unattended = true;
	auto t = CreateScsiTarget("daynaport", 4, o);
	REQUIRE(asDayna(t));
	CHECK(std::string(asDayna(t)->backend().name()) == "none");
	CHECK_FALSE(asDayna(t)->backend().linkUp());

	o.eth = "none";
	CHECK(std::string(asDayna(CreateScsiTarget("daynaport", 4, o))->backend().name()) == "none");
#if HAVE_SLIRP
	o.eth = "slirp";
	auto s = CreateScsiTarget("daynaport", 4, o);
	CHECK(std::string(asDayna(s)->backend().name()) == "slirp");
	// interactive default stays slirp
	ScsiTargetOptions i;
	auto d = CreateScsiTarget("daynaport", 4, i);
	CHECK(std::string(asDayna(d)->backend().name()) == "slirp");
#endif
}
