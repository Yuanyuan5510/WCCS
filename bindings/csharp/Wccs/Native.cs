using System.Reflection;
using System.Runtime.InteropServices;

namespace Wccs;

/// <summary>Raw C ABI declarations. P/Invoke via source-generated LibraryImport
/// (Native AOT friendly). No algorithm logic crosses this boundary.</summary>
internal static partial class Native
{
    internal const string Lib = "wccs";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate ulong NextU64(IntPtr ctx);

    [StructLayout(LayoutKind.Sequential)]
    internal struct WccsRng
    {
        public IntPtr ctx;
        public IntPtr next_u64;   // function pointer, kept alive by the caller
    }

    static Native()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != Lib) return IntPtr.Zero;

        var env = Environment.GetEnvironmentVariable("WCCS_LIBRARY_PATH");
        if (!string.IsNullOrEmpty(env) && NativeLibrary.TryLoad(env, out var fromEnv))
            return fromEnv;

        string[] names = { "libwccs.dll", "wccs.dll", "libwccs.so", "libwccs.dylib" };
        string[] subs = { "", "build", Path.Combine("build", "Release") };
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 9 && dir != null; ++i, dir = dir.Parent)
        {
            foreach (var sub in subs)
            foreach (var name in names)
            {
                var candidate = Path.Combine(dir.FullName, sub, name);
                if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var h))
                    return h;
            }
        }
        return NativeLibrary.TryLoad(libraryName, assembly, searchPath, out var sys)
            ? sys : IntPtr.Zero;
    }

    [LibraryImport(Lib, EntryPoint = "wccs_new_default")]
    internal static unsafe partial IntPtr NewDefault(
        int n, ulong* w, int L, ulong beta_q, ulong alpha_q,
        int R, ulong seed, ulong stream_id, out int err);

    [LibraryImport(Lib, EntryPoint = "wccs_new")]
    internal static unsafe partial IntPtr New(
        int n, ulong* w, int L, ulong beta_q, ulong alpha_q,
        int R, ulong seed, ulong stream_id, WccsRng rng, out int err);

    [LibraryImport(Lib, EntryPoint = "wccs_sample")]
    internal static partial int Sample(IntPtr ctx, out int err);

    [LibraryImport(Lib, EntryPoint = "wccs_free")]
    internal static partial void Free(IntPtr ctx);

    [LibraryImport(Lib, EntryPoint = "wccs_abi_version")]
    internal static partial int AbiVersion();

    [LibraryImport(Lib, EntryPoint = "wccs_step")]
    internal static partial ulong Step(IntPtr ctx);

    [LibraryImport(Lib, EntryPoint = "wccs_total")]
    internal static partial ulong Total(IntPtr ctx);

    [LibraryImport(Lib, EntryPoint = "wccs_error_string")]
    internal static partial IntPtr ErrorString(int err);
}
