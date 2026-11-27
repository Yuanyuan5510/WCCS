using System.Runtime.InteropServices;

namespace Wccs;

public static class ErrorCodes
{
    public const int OK = 0;
    public const int E_INVAL = 1;
    public const int E_NOMEM = 2;
    public const int E_OVERFLOW = 3;
    public const int E_EMPTY = 4;
    public const int E_NULL = 5;
    public const int E_STATE = 6;
    public const int E_STREAM_CONFLICT = 7;
}

/// <summary>Stable WCCS error surfaced as a managed exception.</summary>
public sealed class WccsException : Exception
{
    public int Code { get; }
    public WccsException(int code) : base(BuildMessage(code)) { Code = code; }

    private static string BuildMessage(int code)
    {
        try
        {
            var p = Native.ErrorString(code);
            var text = p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
            return $"wccs error {code}: {text ?? "unknown error"}";
        }
        catch { return $"wccs error {code}: unknown error"; }
    }
}

/// <summary>RAII owner of one opaque WCCS context. A single handle is not
/// thread-safe; use one instance per thread or add external locking.</summary>
public sealed class Sampler : IDisposable
{
    private IntPtr _ctx;
    private Delegate? _rngDelegate;   // keeps an injected callback alive

    public Sampler(
        IReadOnlyList<ulong> weights,
        int L = 0,
        ulong beta_q = 0,
        ulong alpha_q = 0,
        int R = 1,
        ulong seed = 0,
        ulong stream_id = 0,
        Func<ulong>? rng = null)
    {
        var arr = weights.ToArray();
        unsafe
        {
            fixed (ulong* p = arr)
            {
                if (rng is null)
                {
                    _ctx = Native.NewDefault(arr.Length, p, L, beta_q, alpha_q,
                                             R, seed, stream_id, out int err);
                    Check(err);
                }
                else
                {
                    Native.NextU64 callback = _ => rng();
                    _rngDelegate = callback;
                    var rs = new Native.WccsRng
                    {
                        ctx = IntPtr.Zero,
                        next_u64 = Marshal.GetFunctionPointerForDelegate(callback),
                    };
                    _ctx = Native.New(arr.Length, p, L, beta_q, alpha_q,
                                      R, seed, stream_id, rs, out int err);
                    Check(err);
                }
            }
        }
    }

    private void Check(int err)
    {
        if (err != ErrorCodes.OK || _ctx == IntPtr.Zero)
        {
            _ctx = IntPtr.Zero;
            throw new WccsException(err != ErrorCodes.OK ? err : ErrorCodes.E_NOMEM);
        }
    }

    public int Sample()
    {
        if (_ctx == IntPtr.Zero) throw new WccsException(ErrorCodes.E_NULL);
        var r = Native.Sample(_ctx, out int err);
        if (err != ErrorCodes.OK) throw new WccsException(err);
        return r;
    }

    public ulong Step => _ctx == IntPtr.Zero ? 0 : Native.Step(_ctx);
    public ulong Total => _ctx == IntPtr.Zero ? 0 : Native.Total(_ctx);
    public static int AbiVersion => Native.AbiVersion();

    public void Dispose()
    {
        if (_ctx != IntPtr.Zero)
        {
            Native.Free(_ctx);
            _ctx = IntPtr.Zero;
        }
        _rngDelegate = null;
        GC.SuppressFinalize(this);
    }

    ~Sampler() { if (_ctx != IntPtr.Zero) Native.Free(_ctx); }
}
