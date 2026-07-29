# nlohmann/json provenance

- Project: nlohmann/json
- Version: `v3.12.0`
- Release: <https://github.com/nlohmann/json/releases/tag/v3.12.0>
- Vendored source URL: <https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp>
- License URL: <https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT>
- License: MIT
- Import date: 2026-07-27
- `json.hpp` SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- `LICENSE.MIT` SHA-256: `46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603`

The single-header parser and its license are tracked in this directory so an
`htsim_ocs` clean build does not download code and does not rely on a system
JSON package. Phase 03 must configure the parser with exceptions disabled or
translate every parser exception at the CLI boundary, and must perform the
contract's duplicate-key check before typed materialization.
