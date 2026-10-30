# Third-Party Dependencies

Every third-party package or asset added to this repository is recorded here with its
**name**, **version**, and **license identifier** (Requirement 0.7). The license of each
dependency is checked before it is added; content whose license forbids redistribution
is not committed (portfolio rules).

> The project's own code is under the [MIT License](LICENSE). There is no packaging or
> release. Unreal Engine 5.8, ROS 2 Jazzy and `rosbridge_suite` are required tools, not
> redistributed here; the committed level (`RoverLevel.umap`) uses only engine basic
> shapes and spawns its content from `config/level.yaml`.

| Name | Version | License identifier |
| --- | --- | --- |
| numpy | 2.5.4 | BSD-3-Clause (bundled 0BSD, MIT, Zlib, CC0-1.0) |
| matplotlib | 3.11.2 | PSF-based matplotlib license |
| PyYAML | 6.0.3 | MIT |
| rosbags | 0.11.9 | Apache-2.0 |
| Pillow | 13.0.0 | MIT-CMU |
| pytest | 9.1.1 | MIT |
| Hypothesis | 6.170.9 | MPL-2.0 |

## GitHub Actions (CI only, `.github/workflows/ci-sensor-sim-sandbox.yml`)

These run on GitHub's hosted runners for the lint and the analysis tests; none of them is
part of the analysis environment above. Licenses were read from each repository's
`LICENSE` and from PyPI.

| Name | Version | License identifier |
| --- | --- | --- |
| actions/checkout | v7.0.1 (`3d3c42e5aac5ba805825da76410c181273ba90b1`) | MIT (github.com/actions/checkout LICENSE) |
| actions/setup-python | v7.1.0 (`6d4fd22b20465ccc0f7354476588e8df7c9ca0e0`) | MIT (github.com/actions/setup-python LICENSE) |
| Ruff (`pip install ruff==0.17.3` in the lint job; also the lint command in `CONTRIBUTING.md`) | 0.17.3 | MIT (PyPI metadata for 0.17.3: License-Expression MIT) |
