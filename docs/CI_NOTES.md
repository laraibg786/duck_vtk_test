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

## 12. Node 20 deprecation warning is upstream, not ours

Every run annotates:

> Node.js 20 is deprecated. The following actions target Node.js 20 but are being
> forced to run on Node.js 24: actions/checkout@v4

That comes from `_extension_distribution.yml` using `actions/checkout@v4`. Nothing to
fix on our side.
