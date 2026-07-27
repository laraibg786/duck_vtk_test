# How the DuckDB community-extensions pipeline actually behaves

Observed by running it, not inferred from docs. Recorded because several of these are
non-obvious and would otherwise be rediscovered the hard way.

Source of truth: `duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml`
and `config/distribution_matrix.json`, plus `scripts/modify_distribution_matrix.py`.

---

## 1. Platform selection has three independent gates

`excluded_platforms` is only one of them. `should_run()` in
`scripts/modify_distribution_matrix.py` applies, in order:

```python
if arch in excluded_arch_values:                       return False   # your exclude list
if reduced_ci_mode and not config["run_in_reduced_ci_mode"]: return False
if config["opt_in"] and arch not in opt_in_arch_values: return False   # opt-in archs
```

So a platform absent from your exclude list can still never build, because it is
marked **`opt_in: true`** in the matrix config and you did not pass it via
`opt_in_archs`.

From `v1.5-variegata/config/distribution_matrix.json`:

| arch | `run_in_reduced_ci_mode` | `opt_in` |
|---|---|---|
| `linux_amd64` | true | false |
| `linux_arm64` | false | false |
| `linux_amd64_musl` | false | **true** |
| `linux_arm64_musl` | false | **true** |
| `osx_amd64` | false | false |
| `osx_arm64` | true | false |
| `windows_amd64` | true | false |
| `windows_arm64` | false | **true** |
| `windows_amd64_mingw` | true | false |
| `wasm_mvp` | true | false |
| `wasm_eh` / `wasm_threads` | false | false |

Practical consequence: `linux_*_musl` and `windows_arm64` are **not built unless you
opt in** — on this line. Excluding them here is redundant *for v1.5 only*; see §2,
which is why the descriptor names them anyway.

Also note `windows_amd64_rtools` appears in some extensions' `excluded_platforms`
strings but is **not in the v1.5 matrix at all**, so excluding it is a no-op.

## 2. The LTS line does not use the same matrix

`v1.4-andium/config/distribution_matrix.json` marks `linux_amd64_musl` as
**`opt_in: false`**, where `v1.5-variegata` marks it `true`. Confirmed by observation:
in one run, the v1.4.5 job spawned a `linux_amd64_musl` build and the v1.5.5 job did
not, from identical inputs.

So a musl build happens on the LTS line whether or not you ask for it. If musl is not
supported, it must be named in `excluded_platforms` explicitly — relying on the
opt-in default is only correct for v1.5.

## 3. macOS / Windows / Wasm run *after* Linux

The `macos:` job (and Windows, Wasm) declare `needs:` on the Linux job. Right after a
push you therefore see **only Linux jobs**, and the absence of macOS jobs means
"queued behind Linux", not "excluded". Do not spend time debugging the matrix on that
basis — as I briefly did.

Each group is additionally gated on its matrix being non-empty:

```yaml
if: ${{ needs.generate_matrix.outputs.linux_matrix != '{}' && ... != '' }}
```

## 4. linux_arm64 is built but NOT tested

```yaml
if: ${{ matrix.duckdb_arch != 'linux_arm64' && inputs.skip_tests == false }}
```

Both test steps carry that condition, because arm64 runs under emulation. A green
`linux_arm64` therefore means **it compiled and linked**, not that the suite passed.
Worth knowing before treating arm64 as verified.

## 5. `reduced_ci_mode` defaults to `auto`

Which the parse script resolves to `False` — i.e. the full matrix. Passing
`enabled` cuts to the `run_in_reduced_ci_mode` subset (`linux_amd64`, `osx_arm64`,
`windows_amd64`, `windows_amd64_mingw`, `wasm_mvp`). Useful for a first smoke run on a
throwaway repo, since VTK builds from source on every platform.

## 6. A `duckdb` submodule is genuinely required

The workflow checks out the extension repo with `submodules: recursive`, then runs
`make set_duckdb_version`, which is:

```make
set_duckdb_version:
	cd duckdb && git checkout $(DUCKDB_GIT_VERSION)
```

With no `duckdb/` directory that step fails outright. There is an
`override_duckdb_repository` input that instead calls `set_duckdb_repository`
(`rm -rf duckdb && git clone …`), but community-extensions does not set it.

Note the consequence: **your submodule pin does not decide what they build.** They
check DuckDB out to their target version themselves. The pin only determines what a
local build produces, which is why this repo derives `DUCKDB_VERSION_TAG` from the
submodule SHA rather than hardcoding it.

## 7. Dependencies must come from vcpkg

`VCPKG_TOOLCHAIN_PATH` is set from a vcpkg clone pinned per DuckDB version
(`84bab45d…` for v1.5.5, `ce613c41…` for v1.4.x). Nothing on the runner may be
assumed — hence `vcpkg.json` and the `vtk-minimal` overlay port.

`VCPKG_BINARY_SOURCES` defaults to a **read-only** public cache
(`http,https://vcpkg-cache.duckdb.org,read`), so our own port gets no cache hits and
builds from source on every platform, every run. That is the dominant cost of the
`distribution` job and the most likely cause of a timeout.

## 8. Versions, as of this writing

| | |
|---|---|
| default DuckDB | `v1.5.5`, ci_tools `v1.5-variegata` |
| LTS (Andium) | `v1.4.5`, ci_tools `v1.4-andium` |
| next | `main` / `main` |

The Andium and next workflows in community-extensions are currently `if: false`, so
only the default line builds on a submission PR.

## 9. A port's `description` must not contain blank lines

vcpkg serialises a port's `description` into a Debian-style control paragraph, in which
a **blank line terminates the paragraph**. A multi-line description expressed as a JSON
array with `""` entries as separators therefore produces genuinely blank lines and
fails vcpkg's own sanity check — *after* the port has finished building:

```
[sanity check] Failed to parse a serialized binary paragraph.
vcpkg::serialize(const BinaryParagraph&, std::string&):8:5:
  error: unexpected end of line, to span a blank line use "  ."
```

Two things make this expensive to debug. The message names `vcpkg::serialize`, not your
port, and suggests re-bootstrapping vcpkg (which does not help). And it appears at the
very end of a ~13-minute VTK build, so each iteration is slow.

Keep `description` to a single line and put the rationale in `portfile.cmake`. Blank
lines would have to be encoded as `" ."`, which is not worth the fragility.

## 10. A submodule's `.git` may be a FILE, and its pointer is relative

Relevant to anything that bind-mounts or copies a submodule directory.

`git submodule update` produces a working tree whose `.git` is a **file** containing
`gitdir: ../.git/modules/<name>`. A plain `git init` inside the directory produces a
`.git` **directory**. Both are valid; they behave differently when the directory is
moved or mounted in isolation, because the relative pointer escapes:

```
fatal: not a git repository: /mirror/citools/../.git/modules/extension-ci-tools
```

Resolve it first — `git -C <dir> rev-parse --absolute-git-dir` yields a real repository
path for both shapes — and mount/copy that.

This bites specifically when local and CI differ in how the submodule was created, as
happened here: local used `git init`, CI used `git submodule update`, so the bug
reproduced only in CI.

## 11. `cancel-in-progress` will cancel your own run

Obvious in hindsight. With

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

a second push cancels the first run mid-flight, and every job reports `cancelled`
rather than `failure`. That is easy to misread as an infrastructure problem. Do not push
while a run you care about is in flight — the community-extensions Linux jobs take
~20 minutes and are the only coverage of a custom vcpkg port.

## 12. The per-OS matrices are fail-fast, and that HIDES information

Each platform group (Windows, macOS, Linux) is one matrix, and it is fail-fast. So a
failure in a target you do not care about **cancels the sibling you do**:

```
failure    Windows (windows_amd64_mingw, x64-mingw-static)
cancelled  Windows (windows_amd64,      x64-windows-static-release)   <- collateral
```

That happened on both DuckDB lines, twice in a row, and it cost two full cycles: MSVC
never ran to completion, so its status stayed unknown while looking like it had been
tried. `cancelled` and `failure` are different words for a reason — read them
carefully, and if a platform you need keeps getting cancelled, drop the sibling that
keeps failing rather than re-running and hoping.

## 13. `windows_amd64_mingw` fails on DuckDB's own code

Not on your extension. Under the rtools42 mingw toolchain:

```
duckdb/tools/sqlite3_api_wrapper/sqlite3_api_wrapper.cpp:1503
  objidl.h: error: reference to 'byte' is ambiguous
    candidates are: 'enum class std::byte' / 'typedef unsigned char byte' (rpcndr.h)
```

`sqlite3_api_wrapper.cpp` includes `<windows.h>` after DuckDB's headers have already
pulled in `std::byte`. Fixing it means patching DuckDB, so the practical answer is to
exclude the arch — which is what the ecosystem does. `h5db` (also a heavy C++
file-format dependency) excludes it, and so does `iceberg`, a **core** extension
maintained by DuckDB. Check descriptors upstream before assuming a failure is yours:

```bash
gh api repos/duckdb/community-extensions/contents/extensions/h5db/description.yml   --jq '.content' | base64 -d | grep excluded_platforms
```

## 14. `requires_toolchains` does NOT control vcpkg

Some descriptors say `requires_toolchains: "vcpkg;python3"`, which suggests vcpkg is
opt-in. It is not. The field maps to the reusable workflow's `extra_toolchains`, which
is only forwarded to the Linux Docker image build and to rust log collection. vcpkg is
provisioned unconditionally, in `extension-ci-tools/docker/<arch>/Dockerfile`:

```dockerfile
ARG vcpkg_url
ARG vcpkg_commit
RUN mkdir /vcpkg && ... git checkout $vcpkg_commit && ./bootstrap-vcpkg.sh
ENV VCPKG_TOOLCHAIN_PATH=/vcpkg/scripts/buildsystems/vcpkg.cmake
```

So a port-only extension needs no `requires_toolchains` at all. Declaring `vcpkg`
there is harmless but redundant; declare only genuinely extra toolchains (python3,
rust).

## 15. Distro-packaged VTK is a trap, and it fails SILENTLY

Not strictly a CI note, but it was CI that caught it, and only because the matrix
happened to include a job building against the distro package.

Ubuntu 24.04's `libvtk9-dev` (9.1.0) cannot parse XML files containing an
`<AppendedData>` section — which is what most real VTK writers emit. It reports

```
vtkXMLDataParser: Error parsing XML in stream ...: junk after document element
vtkXMLReader:     Error parsing input file.  ReadXMLInformation aborting.
```

and then **leaves `GetErrorCode()` at Success and hands back a valid but EMPTY
dataset**. A mesh of 2903 points reads as 0 points with no error anywhere.

Two lessons. Never trust `GetErrorCode()` on a VTK reader — capture the output window
instead. And a job that builds against a distro package to prove "we are not tied to
the newest version" can pass for months while proving the opposite; ours did.

## 16. Node 20 deprecation warning is upstream, not ours

Every run annotates:

> Node.js 20 is deprecated. The following actions target Node.js 20 but are being
> forced to run on Node.js 24: actions/checkout@v4

That comes from `_extension_distribution.yml` using `actions/checkout@v4`. Nothing to
fix on our side.

## 17. A matrix cannot drive `uses:` — and getting it wrong fails with NO failing job

The most expensive one so far, because the failure mode actively misleads you.

We expressed both DuckDB lines as one `distribution` job with a matrix over
`duckdb_version` / `ci_tools_version`, keeping a single
`uses: …/_extension_distribution.yml@v1.5-variegata`. GitHub resolves
reusable-workflow references **before** matrix expansion, so every matrix entry ran
against the v1.5 workflow regardless of the `ci_tools_version` it was handed.

That is not merely redundant. The two releases disagree about where the macOS runner
label comes from:

| | `v1.5-variegata` | `v1.4-andium` |
|---|---|---|
| `macos:` job | `runs-on: ${{ matrix.runner }}` | `runs-on: macos-latest` (hardcoded) |
| `osx` entries in `distribution_matrix.json` | include `"runner": "macos-15"` | **no `runner` key at all** |

So the v1.4.5 entry fed v1.4-andium's config into v1.5's workflow, `runs-on`
evaluated to empty, and GitHub **refused to create the job**. The observable result:

```
$ gh run view <id> --json jobs --jq '.jobs[] | "\(.conclusion)\t\(.name)"'
success  …            # all 16 jobs
skipped  …
$ gh api …/runs/<id> --jq .conclusion
failure                # the run itself
```

Sixteen jobs, none failed, run red, and the v1.4.5 **MacOS group absent entirely** —
no record to click into. Easy to misread as a GitHub outage.

**The rule: the `@ref` and the `ci_tools_version` input must name the same release.**
Since `uses:` cannot be templated, supporting two lines means two separate top-level
jobs, which is what `MainDistributionPipeline.yml` now does and what every extension
in the ecosystem that builds the LTS line does — a survey of all 293 listed community
extensions found 6 building the andium line, all 6 pinning `@v1.4-andium`, and **zero**
using a matrix here. `scripts/submit_check.sh` now fails on a mismatch.

Corollary for diagnosis: when a run is red but no job is, look for a job group that is
**missing** rather than failing, and check `runs-on` for an empty expression.

## 18. Only 6 of 293 extensions build the LTS line in their own CI

Worth knowing before spending CI minutes on it: community-extensions'
`build_andium.yml` is `if: false`, so **the LTS line is not built for a submission PR
at all**. Anything you learn from an andium job in your own repo is early warning for
when it is re-enabled, not submission evidence.

The descriptor hook for it is `repo.andium`, read by their `scripts/build.py` only when
`DUCKDB_VERSION == v1.4.5`. 72 of 293 descriptors set it; 65 point it at a **different**
commit from `repo.ref` (a separate branch per line) and 7 at the same commit (one tree
serving both). `repo.ref` itself is a raw 40-char SHA in 269 of 293 — not a branch, so
that what upstream rebuilds later is what was reviewed.
