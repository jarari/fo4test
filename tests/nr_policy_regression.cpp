#include "../src/FeatureRetryPolicy.h"
#include "../src/NRPassOptions.h"
#include "../src/TextureMemoryReserve.h"
#include <cstdio>
#include <limits>

constexpr bool PassSettings()
{
	using namespace nvngx::dlss_nr;
	Options common;
	common.preset = 1;
	common.style = 2;
	common.intensity = 0.4f;
	common.localToneStrength = 1.5f;
	common.localStructureStrength = 0.3f;
	common.skinStructureStrength = -1.0f;
	common.useAutoMask = true;
	auto additional = DefaultAdditionalPassOptions();
	additional[0].preset = 2;
	additional[0].intensity = 0.7f;
	additional[0].localToneStrength = 0.25f;
	additional[1].preset = 3;
	additional[1].intensity = 0.9f;
	for (bool separate : {false, true}) {
		if (ResolvePassOptions(common, separate, additional, 0) != common) return false;
	}
	for (unsigned pass = 1; pass < 3; ++pass) {
		auto legacy = common;
		legacy.localToneStrength = 0;
		if (ResolvePassOptions(common, false, additional, pass) != legacy) return false;
		if (ResolvePassOptions(common, true, additional, pass) != additional[pass - 1]) return false;
	}
	return true;
}

constexpr bool RetainedBudget()
{
	using namespace TextureMemoryReserve;
	Configuration c;
	c.width = 1920; c.height = 1080;
	c.renderWidth = 1280; c.renderHeight = 720;
	c.sr = SR::DLSS; c.nrPasses = 2; c.nrAfterSR = true;
	const auto active = Calculate(c);
	// Feature/guide and scratch allocations survive disabling NR.
	c.retainedNRBytes = active.nr;
	c.retainedNRColorBytes = uint64_t{c.slots} * c.width * c.height * c.colorBytes;
	if (Calculate(c).reserve != active.reserve) return false; // no double count
	c.nrPasses = 0;
	if (Calculate(c).reserve != active.reserve) return false; // Off keeps headroom
	c.nrPasses = 1;
	if (Calculate(c).reserve < active.reserve) return false; // old second pass remains
	c.nrPasses = 0;
	c.sr = SR::None;
	if (Calculate(c).nr < c.retainedNRBytes + c.retainedNRColorBytes) return false;
	c.sr = SR::DLSS;
	c.retainedNRBytes = c.retainedNRColorBytes = 0;
	if (Calculate(c).reserve >= active.reserve) return false; // real release returns it
	c.retainedNRBytes = 8ull * 1024 * MiB;
	return Calculate(c).reserve == MaxReserve;
}

constexpr bool TransientBackoff()
{
	using namespace FeatureRetryPolicy;
	unsigned stage = 0;
	for (unsigned i = 0; i < 1000; ++i) {
		stage = NextAttempt(stage);
		if (DelayMilliseconds(stage) < 2000 || DelayMilliseconds(stage) > 30000) return false;
	}
	return DelayMilliseconds(1) == 2000 && DelayMilliseconds(2) == 4000 &&
		DelayMilliseconds(3) == 8000 && DelayMilliseconds(4) == 16000 &&
		DelayMilliseconds(stage) == 30000 &&
		DelayMilliseconds(std::numeric_limits<unsigned>::max()) == 30000;
}

static_assert(PassSettings());
static_assert(RetainedBudget());
static_assert(TransientBackoff());
int main()
{
	if (!PassSettings() || !RetainedBudget() || !TransientBackoff()) return 1;
	std::puts("PASS: separate/shared pass settings; retained NR budget; bounded transient backoff.");
}
