// ScriptedTarget: a ScsiTarget for unit tests.  Records every CDB it is
// given and plays canned steps.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "devices/scsi_target.h"

class ScriptedTarget : public scsi::ScsiTarget
{
public:
	struct Plan
	{
		scsi::Step::Kind kind = scsi::Step::StatusPhase;
		std::vector<uint8_t> inData; // DataIn payload
		size_t outLen = 0;			 // DataOut record length
		scsi::Status status = scsi::Status::Good;
	};
	struct Call
	{
		uint8_t lun;
		std::vector<uint8_t> cdb;
	};

	static Plan dataIn(std::vector<uint8_t> d) { return Plan{scsi::Step::DataIn, std::move(d), 0, {}}; }
	static Plan dataOut(size_t n) { return Plan{scsi::Step::DataOut, {}, n, {}}; }
	static Plan status(scsi::Status s = scsi::Status::Good) { return Plan{scsi::Step::StatusPhase, {}, 0, s}; }

	// Plans consumed in order by command(), dataOutDone() and dataInDone().
	std::deque<Plan> plans;
	std::vector<Call> calls;
	std::vector<std::vector<uint8_t>> received; // completed DataOut records
	int busResets = 0, aborts = 0;

	scsi::Step command(uint8_t lun, std::span<const uint8_t> cdb) override
	{
		calls.push_back({lun, std::vector<uint8_t>(cdb.begin(), cdb.end())});
		return next();
	}
	scsi::Step dataOutDone() override
	{
		received.push_back(outBuf_);
		return next();
	}
	scsi::Step dataInDone() override { return next(); }
	void busReset() override { busResets++; }
	void abort() override { aborts++; }
	const char *name() const override { return "scripted"; }

private:
	scsi::Step next()
	{
		if (plans.empty()) return scsi::Step{scsi::Step::StatusPhase, {}, {}, scsi::Status::Good};
		cur_ = plans.front();
		plans.pop_front();
		scsi::Step s;
		s.kind = cur_.kind;
		s.status = cur_.status;
		if (cur_.kind == scsi::Step::DataIn) s.in = cur_.inData;
		if (cur_.kind == scsi::Step::DataOut)
		{
			outBuf_.assign(cur_.outLen, 0);
			s.out = outBuf_;
		}
		return s;
	}
	Plan cur_;
	std::vector<uint8_t> outBuf_;
};
