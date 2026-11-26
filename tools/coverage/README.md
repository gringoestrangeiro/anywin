# Coverage data

`gen.py` builds the map and tables from two snapshots in `data/`:

## `win32api.json`

The denominator: every function Microsoft declares in
[`microsoft/win32metadata`](https://github.com/microsoft/win32metadata), the Microsoft-maintained
metadata behind the Win32 API projections.

- Release used: **`v71.0.45-preview`**, published 2026-11-05.
- Package: `Microsoft.Windows.SDK.Win32Metadata.71.0.45-preview.nupkg`
  (asset of that release).
- File read: `Windows.Win32.winmd`.
- Selection: every `MethodDef` with a P/Invoke import (`ImplMap`), using the import name (entry
  point) when present. Duplicate DLL/name pairs are removed.
- Result: 18,319 entry points across 368 DLLs. API-set DLLs are folded into their base module
  with the same rules as `runtime/modmap.h`, so `api-ms-win-core-*` counts as `kernel32`, etc.

This covers documented exported Win32 functions. It does not include COM vtable methods,
undocumented exports, or the C runtime (`ucrtbase`/`msvcrt`); those are reported separately by
the generator.

To refresh it, download that NuGet package, extract `Windows.Win32.winmd`, and walk its
`MethodDefinition` table with any ECMA-335 metadata reader:

```csharp
using var pe = new PEReader(File.OpenRead("Windows.Win32.winmd"));
var md = pe.GetMetadataReader();
foreach (var h in md.MethodDefinitions) {
    var m = md.GetMethodDefinition(h);
    var imp = m.GetImport();
    if (imp.Module.IsNil) continue;
    string dll = md.GetString(md.GetModuleReference(imp.Module).Name);
    string name = imp.Name.IsNil ? md.GetString(m.Name) : md.GetString(imp.Name);
    // add (dll, name)
}
```

## `corpus.json`

The sample programs used for the import-coverage section. `make coverage` refreshes entries for
every `*.exe` in the repository root and `build/`, then merges them with this snapshot so a clean
checkout without test binaries still has the full corpus.

## Regenerating

```sh
make            # optional: builds libanywin.o and the tool
make test       # optional: builds the full probe corpus under build/
make coverage
```
