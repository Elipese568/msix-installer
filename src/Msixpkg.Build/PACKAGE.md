# Msixpkg.Build

Build-time MSBuild task that packs a **signed single-project MSIX** into a **`.msixpkg`** container
during `dotnet build` / Visual Studio build. No extra CLI step, no manual `makeappx` juggling.

```xml
<ItemGroup>
  <PackageReference Include="Msixpkg.Build" Version="0.1.0" />
</ItemGroup>

<PropertyGroup>
  <MsixpkgEnabled>true</MsixpkgEnabled>
  <PackageCertificateKeyFile>certs\App.Dev.pfx</PackageCertificateKeyFile>
  <!-- Never commit a password: inject it from a command line property or a user-level .props file. -->
  <MsixpkgCertificatePassword Condition="'$(MsixDevCertPassword)' != ''">$(MsixDevCertPassword)</MsixpkgCertificatePassword>
  <MsixpkgDependencyDir>$(AppxPackageDir)Dependencies\</MsixpkgDependencyDir>
</PropertyGroup>
```

The package auto-imports `build\Msixpkg.Build.targets`; the task hooks `_GenerateAppxPackage` (the real
target of the single-project MSIX tooling) and `Build`, so both `dotnet build` and Visual Studio produce
the container.

## What you get

| Output | Meaning |
|---|---|
| `$(AppxPackageDir)<Name>_<Version>_<Arch>.msixpkg` | The container: inner `.msix`, dependency packages, publisher certificate and an Ed25519/RSA-signed manifest |
| `<...>.cer` | The publisher certificate exported from the signing `.pfx` |

## Properties (all optional except `MsixpkgEnabled`)

| Property | Default | Meaning |
|---|---|---|
| `MsixpkgEnabled` | `false` | Master switch; the task does nothing unless this is `true` |
| `MsixpkgInputMsix` | auto | Explicit input `.msix`. Omit it and the task resolves the Appx build output itself |
| `MsixpkgCertificatePfx` | `$(PackageCertificateKeyFile)` | The same `.pfx` that signed the `.msix` |
| `MsixpkgCertificatePassword` | `$(PackageCertificatePassword)` | Password for that `.pfx` |
| `MsixpkgDependencyDir` | — | Directory holding framework dependency `.msix` files (`Dependencies\`) |
| `MsixpkgOutputDir` | `$(AppxPackageDir)` | Where the `.msixpkg` is written |
| `MsixpkgCreatedUtc` | now | Fixed container timestamp, e.g. `2026-01-01T00:00:00Z`. Setting it makes the build **byte-for-byte reproducible** |
| `MsixpkgSign` | `true` | Sign the container |
| `MsixpkgIncludeCertificate` | `true` | Embed the publisher certificate |
| `MsixpkgAllowAllUsers` | `true` | Allow machine-wide installation |
| `MsixpkgMinOsBuild` | `19041` | Minimum Windows build recorded in the manifest |
| `MsixpkgInstallerMinVersion` | `0.1.0` | Minimum installer version required by the container |
| `MsixpkgDisplayName`, `MsixpkgDisplayDescription`, `MsixpkgDisplayHomepage` | from the `.msix` manifest | Strings shown by the installer wizard |

The task also sets `MsixpkgOutputFile`, `MsixpkgCertificateFile`, `MsixpkgPublisherThumbprint` and
`MsixpkgPackageIdentity` output items.

## Requirements

* Any MSBuild host that can load a `netstandard2.0` task: Visual Studio's .NET Framework MSBuild **and**
  the .NET SDK MSBuild (`dotnet build`) are both supported.
* A **signed** `.msix` whose signing certificate subject matches `Identity/@Publisher` character for
  character — the task fails with an actionable error instead of producing an uninstallable container.

## What is inside the package

```
build\Msixpkg.Build.targets                 auto-imported by NuGet
tasks\netstandard2.0\Msixpkg.Tasks.dll      the MSBuild task
tasks\netstandard2.0\Msixpkg.Core.dll       bundled binary (container reader/writer)
```

`Msixpkg.Core.dll` is shipped **inside** the package on purpose: the task assembly is loaded by MSBuild
in its own load context, so its dependency has to sit next to it. Do not add a separate `Msixpkg.Core`
package reference — there is no such package, and a second copy of the assembly would only create
version conflicts inside the MSBuild process.

## License

MIT. The container format, the native installer and the packer CLI live in the same repository but are
**not** distributed through NuGet.
