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
opt in**. Excluding them, as we initially did, was redundant.

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

---

## 17. The whole `description.yml` is published verbatim — comments included

`duckdb/community-extensions/scripts/generate_md.sh` contains literally:

```sh
cat extensions/$extension/description.yml >> $EXTENSION_README
```

so the file is appended into the Jekyll front matter of
`duckdb.org/community_extensions/extensions/<name>`. **Every comment line ships to a
public page.** Our descriptor carried ~60 lines of internal CI reasoning; that is why
`community-extension/description.yml` is now lean and this file holds the reasoning.

Related format rules, all verified against `scripts/build.py` in that repo:

* `excluded_platforms` and `requires_toolchains` must be `;`-separated **strings**.
  A YAML list serialises as `['wasm_mvp', ...]`, matches no platform, and raises no
  error. One live extension (`mssql`) has this bug and excludes nothing.
* The directory name must equal `extension.name` — this is one of only two explicit
  `raise ValueError`s in their entire validation path. The other rejects a PR that
  touches more than one descriptor.
* The file must be `description.yml`, not `.yaml`. `build.yml`'s path filter is
  `extensions/*/description.yml`, so a wrongly-named file runs **no CI at all** and
  the PR simply sits there.
* `repo.ref` must be a full 40-character SHA. Maintainers reject tags explicitly:
  "we do not allow (mutable) tags as `ref` targets, only hashes."
* `maintainers` entries are bare GitHub handles — no `@`, no email, no display name.
* `extension.version` is **not read by any script** and is explicitly deprioritised
  by maintainers; 12 live extensions omit it entirely.
* There is no JSON schema and no validator beyond `scripts/build.py`, and no
  `CONTRIBUTING.md` or PR template. The prose spec lives in `duckdb/duckdb-web` at
  `community_extensions/documentation.md` (which spells the key `licence`; 317 of
  319 live descriptors use `license`, and no script reads either).

## 18. `docs.hello_world` is documentation — it is never executed

Worth recording because it is easy to assume otherwise and over-engineer the example.
`grep -rn hello_world` across all of `community-extensions` returns exactly three
hits: one in `generate_extensions_json.py` (copies the string into JSON) and two in
`layout/default.md` (renders it inside a ```sql fence). No script that invokes DuckDB
touches it, and the `doc_test` job in `build.yml` is currently `if: false`.

Two consequences:

* A remote URL in `hello_world` introduces no network dependency into their pipeline.
* The layout **already wraps it in a ```sql fence**, so wrapping it again in the
  descriptor produces a visibly broken code block. Maintainers have commented on this.

A human reviewer does read the example, though, and has pushed back on ones that do
not work. Ours therefore leads with a local path and mentions the `httpfs`/`https://`
route as a trailing comment.

## 19. A run can report `failure` with every visible job green

Run `30218995966` on the throwaway test repo concluded `failure` while all **16**
check runs were `success` or `skipped`. The cause is that
`community-extensions build (v1.4.5) / MacOS` produced **no job record at all** — not
`failure`, not `cancelled`, simply absent from both `gh run view --json jobs` and the
commit's check-runs list.

The caller job of a reusable workflow is not exposed as a check run, so when the
called workflow fails to materialise a job, the failure has nowhere to appear. The
relevant difference between the two branches:

* `v1.4-andium`   → `macos:` job has `runs-on: macos-latest` (hardcoded)
* `v1.5-variegata` → `runs-on: ${{ matrix.runner }}` (pinned `macos-15`)

Practical lesson, and a companion to §12: **counting green checks does not tell you
the run passed.** Compare the set of jobs that ran against the set you expected.
Since `build_andium.yml` is `if: false` upstream (§8), the LTS macOS line is not
built for a submission anyway.

## 20. VTK 9.7.0 is refused, on measurement

VTK 9.7.0 (2026-08-15) is the current stable release and VTK 9.6 is end-of-life —
there will be no 9.6.3. We are nevertheless pinned to `>= 9.6, < 9.7`, because 9.7.0
removes the only signal this extension has for truncated legacy ASCII files.

Measured with a standalone probe linked against each version in turn, reading
`test/data/synthetic/truncated.vtk` (header declares `POINTS 27`, file ends after 4):

| | VTK 9.6.2 | VTK 9.7.0 |
|---|---|---|
| vtkOutputWindow message | `WARN| Error reading ascii data. Possible mismatch of datasize with declaration.` | **none** |
| `GetErrorCode()` | `0 (Success)` | `0 (Success)` |
| points returned | 27 | 27 |
| bounds | `-1.56682e+06` (uninitialised) | `0..1` (zero-filled) |

`GetErrorCode()` is Success in both — it has never been trustworthy here, which is
why `src/vtk/vtk_error_scope.cpp` captures `vtkOutputWindow` instead. Under 9.7.0
there is nothing left to capture, so `ATTACH` succeeds and returns 27 points, 23 of
which were never in the file. `test/sql/attach_errors.test:14` catches this.

Scope, from the same probe:

* Affected: malformed legacy **ASCII** data — `truncated.vtk`, `bad_ascii_nan.vtk`.
* Not affected: valid files (`uGridEx.vtk`, `office.binary.vtk` read identically in
  both), unrecognised file types, and **XML** truncation, which 9.7.0 still reports.

This is the same shape as the VTK 9.1 appended-data trap in §15 and the reason the
floor exists at all. `cmake/DuckVTKFindVTK.cmake` therefore has a hard upper bound
as well as a lower one, and it is a `FATAL_ERROR` rather than a warning because the
failure it prevents is silent.

Other 9.7.0 facts confirmed while evaluating it, none of which changed the verdict:

* It configures and builds cleanly with our exact renderless/minimal module set.
* The transitive closure grows by exactly one library, `vtkCommonCache`, pulled in
  as a `PRIVATE_DEPENDS` of `FiltersCore`.
* `VTK_USE_PCH` is new and defaults to `ON`; the `vtkArrayBulkInstantiate_*` TUs hold
  ~1.4 GB RSS each, so `-j8` OOMs on a 15 GB machine where `-j4` is fine.
* Its tarball SHA512 was verified independently:
  `a60c0a76...faf82ea71` for `VTK-9.7.0.tar.gz` (58,568,819 bytes).

**To lift the ceiling**, build the candidate VTK, point the extension at it, and
require `test/sql/attach_errors.test` to pass. Do not raise it on release notes alone.

## 21. A captured stdout stream can blow up the environment (E2BIG)

Self-inflicted, but the failure mode is worth knowing because the error message
points nowhere near the cause.

`scripts/build_minimal_vtk.sh` documents that stdout carries exactly one line, the
cmake config directory, so callers can do `VTK_DIR=$(build_minimal_vtk.sh)` and let
`set -e` abort on failure. Progress output was moved to stderr for this — but
**cmake's own stdout was not**, and only the cache-MISS path reaches cmake. Against a
warm `~/.local/vtk-9.6.2` the script returns early and prints one line, so this could
not reproduce locally.

Two call sites, two different symptoms from the same cause:

* GitHub Actions (`quick`): the multi-line value went into `$GITHUB_ENV` —
  `Unable to process file command 'env' successfully.`
  `Invalid format '-- The CXX compiler identification is GNU 14.2.0'`
* Docker (`cold boot`): `export VTK_DIR=<megabytes>` pushed the environment past
  `MAX_ARG_STRLEN`, so every subsequent `execve` failed —
  `/usr/bin/tail: Argument list too long`, then `rm`, then
  `make: *** [ci-verify] Error 126`. Exit 126 is "cannot execute", which reads like
  a permissions problem and sent the first diagnosis in the wrong direction.

Lessons:

1. If a script's contract is "stdout is data", every command in it must be audited,
   not just the ones that obviously print. `cmake`, `make` and `git` all write
   progress to stdout.
2. Test the SLOW path. A contract that only holds on the early-return branch is not
   a contract. The verification that mattered was running the script into a fresh
   prefix so configure/build/install actually executed.
3. `Argument list too long` from a command that plainly exists means an oversized
   environment, not a missing binary.
