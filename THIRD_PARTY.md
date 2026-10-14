# Third-Party Dependencies

> Licenses must be **re-verified against the exact pinned versions** listed
> here. License identifiers use SPDX where a standard identifier exists.
> Versions match the pins in `pyproject.toml` and `requirements.txt`.

| Name | Version | License identifier |
|---|---|---|
| openai | 3.29.0 | Apache-2.0 |
| httpx | 0.28.1 | BSD-3-Clause |
| numpy | 2.4.6 | BSD-3-Clause |
| scipy | 1.15.0 | BSD-3-Clause |
| matplotlib | 3.9.2 | PSF-2.0 (matplotlib BDFL license, PSF-based, BSD-compatible) |
| pytest | 9.1.1 | MIT |
| pytest-cov | 7.1.0 | MIT |
| hypothesis | 6.169.0 | MPL-2.0 |

## SciPy vs statsmodels decision

The honest-statistics layer (Wilson interval + bootstrap resampling, Req 6.2)
uses **SciPy**, not statsmodels (decision fixed in the build plan). SciPy was
proven installable on Python 3.11.5 against the pinned `numpy==2.4.6`:

- The build-plan candidate `scipy==1.14.1` **conflicts** with `numpy==2.4.6`
  (`scipy 1.14.x depends on numpy<2.3,>=1.23.5`), confirmed via
  `pip install --dry-run`.
- `scipy==1.15.0` is the **nearest real release** above the candidate that
  resolves against `numpy==2.4.6`; it is the recorded pin.

SciPy is therefore **kept** (not dropped); no pure-numpy Wilson+bootstrap
fallback is needed.

## Prior-work reuse: AgentDojo / InjecAgent [ME]

> Owner task (M0, Req 1.1-1.3, 0.8). Record the final PriorWork_Decision
> (reuse AgentDojo as the sandbox/attack substrate vs a self-contained custom
> suite), the reuse mode (pinned package version, or vendored named modules
> with per-module attribution), and the exact license string re-verified from
> each repository on the target machine.

| Name | Version | License identifier |
|---|---|---|
| AgentDojo (agentdojo-core) | TBD ([ME], M0) | TBD (re-verify on target machine) |
| InjecAgent | TBD ([ME], M0) | TBD (re-verify on target machine) |

### M1 Sandbox / tools / user-task catalog reuse

The M1 Sandbox, the four Simulated_Tools (`aib/tools.py`), and the full
40-task User_Task catalog with its deterministic success checks (`aib/tasks.py`)
are **original authored material** for this repository. No task text, fixture
body, or success-check ground truth was copied from AgentDojo, InjecAgent, or
any other third-party source. If the owner later imports task or fixture
material from a third-party suite during the `[ME]` refinement of task 2.7,
record its name, version, license, and attribution here.
