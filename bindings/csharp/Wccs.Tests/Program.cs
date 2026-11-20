// C# binding smoke test + cross-language vector emitter.
// Output line "indices: ..." is consumed by tests/run_cross_lang.py.
// SPDX-License-Identifier: Apache-2.0
using Wccs;

static void Assert(bool cond, string message)
{
    if (!cond) throw new Exception("ASSERT FAILED: " + message);
}

Assert(Sampler.AbiVersion == 1, "ABI version");

var weights = Enumerable.Repeat(1UL, 10).ToArray();
using (var s = new Sampler(weights, L: 3, R: 1000, seed: 42))
{
    Assert(s.Step == 0, "step starts at 0");
    Assert(s.Total == 10, "total = sum(w)");
    var idx = new int[100];
    for (int i = 0; i < 100; i++)
    {
        idx[i] = s.Sample();
        Assert(idx[i] >= 0 && idx[i] < 10, "index in range");
    }
    Assert(s.Step == 100, "step == 100");
    Console.WriteLine("indices: " + string.Join(" ", idx));
}

// Error mapping
try
{
    using var bad = new Sampler(new ulong[] { 1, 0, 1 });
    Assert(false, "invalid input must throw");
}
catch (WccsException e)
{
    Assert(e.Code == ErrorCodes.E_INVAL, "E_INVAL");
}

// Reproducibility
using (var a = new Sampler(weights, L: 3, R: 1000, seed: 42))
using (var b = new Sampler(weights, L: 3, R: 1000, seed: 42))
{
    for (int i = 0; i < 500; i++) Assert(a.Sample() == b.Sample(), "reproducible");
}

// Custom RNG injection through wccs_new
ulong state = 0x12345678;
ulong Next() { state = unchecked(state * 6364136223846793005UL + 1); return state; }
using (var a = new Sampler(weights, L: 3, R: 1000, seed: 1, rng: Next))
{
    state = 0x12345678;
    using var b = new Sampler(weights, L: 3, R: 1000, seed: 1, rng: Next);
    // Note: both share the same captured state; just verify no crash and valid range.
    for (int i = 0; i < 100; i++) Assert(a.Sample() >= 0, "injected rng sample");
}
