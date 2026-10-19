/*
	slirp_timers.h — the timer table behind libslirp's timer callbacks.

	libslirp asks the host to create timers (timer_new_opaque), arm them
	(timer_mod, with an expiry in MILLISECONDS on the clock whose value
	clock_get_ns() returns in NANOSECONDS) and free them.  It never fires
	them itself: the host must call slirp_handle_timer() when one is due.
	This table keeps the state; SlirpStack::poll() asks it for the due ones.
	No libslirp dependency, so it is unit-testable.
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

class SlirpTimers
{
public:
	struct Timer
	{
		int id;				// SlirpTimerId, opaque here
		void *cbOpaque;
		int64_t expireMs = -1; // -1 = not armed
	};

	Timer *add(int id, void *cbOpaque)
	{
		timers_.push_back(new Timer{id, cbOpaque});
		return timers_.back();
	}
	void remove(Timer *t)
	{
		timers_.erase(std::remove(timers_.begin(), timers_.end(), t), timers_.end());
		delete t;
	}
	static void arm(Timer *t, int64_t expireMs) { t->expireMs = expireMs; }

	// Number of armed timers.
	size_t armed() const
	{
		return size_t(std::count_if(timers_.begin(), timers_.end(), [](const Timer *t) { return t->expireMs >= 0; }));
	}

	// Disarm and call fire(id, cbOpaque) for each timer due at `nowMs`.  The
	// callback may arm, add or remove timers.  Returns how many fired.
	template <class Fire>
	size_t fireDue(int64_t nowMs, Fire fire)
	{
		const std::vector<Timer *> snapshot = timers_;
		size_t n = 0;
		for (Timer *t : snapshot)
		{
			// an earlier callback may have removed this timer
			if (std::find(timers_.begin(), timers_.end(), t) == timers_.end()) continue;
			if (t->expireMs < 0 || t->expireMs > nowMs) continue;
			t->expireMs = -1;
			fire(t->id, t->cbOpaque);
			n++;
		}
		return n;
	}

	~SlirpTimers()
	{
		for (Timer *t : timers_) delete t;
	}

private:
	std::vector<Timer *> timers_;
};
