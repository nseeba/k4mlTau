# k4mlTau

`k4mlTau` is a Key4hep/FCCSW Gaudi package that runs the ML-τ MultiParTau model
(exported to ONNX) on EDM4hep reconstruction output and writes reconstructed
hadronic tau (τh) candidates – visible 4-momentum, charge, τh ID score and decay
mode – to EDM4hep ROOT files.

The reconstruction chain is:

```text
PandoraPFOs -> mlTauSeedBuilder -> mlTau -> mlTau (particles) + mlTauID (scores)
```

`mlTauSeedBuilder` clusters the reconstructed particles into seed jets (FastJet
ee_genkt, anti-kt, R = 0.4 – the same jets as used in training). `mlTau` builds
the model inputs, runs ONNX Runtime and writes the tau candidates plus their ID
information. Everything runs on CPU.

Input files must contain the collections `PandoraPFOs`, `SiTracks_Refitted` and
`PrimaryVertices` (standard CLD reconstruction).

## Setup

### 1. Clone the repository

```bash
git clone git@github.com:nseeba/k4mlTau.git
cd k4mlTau
```

### 2. Download and unpack the model

Download both assets of the release
[v0.1.0](https://github.com/nseeba/k4mlTau/releases/tag/v0.1.0), e.g. with your
web browser:

```text
mlTau_0612_multipartau_full_b8483f6.tar.gz
mlTau_0612_multipartau_full_b8483f6.tar.gz.sha256
```

Move them into the `models/` directory of the repository and unpack them there
(replace `~/Downloads` with wherever the files are; on a remote machine copy them
there first, see [Running on lxplus](#running-on-lxplus-htcondor)):

```bash
mkdir -p models
mv ~/Downloads/mlTau_0612_multipartau_full_b8483f6.tar.gz* models/
cd models
sha256sum -c mlTau_0612_multipartau_full_b8483f6.tar.gz.sha256   # optional, prints "OK"
tar -xzf mlTau_0612_multipartau_full_b8483f6.tar.gz
cd ..
```

Result:

```text
models/
  mlTau_0612_multipartau_full_b8483f6/
    model.onnx
    metadata.json
```

### 3. Build

```bash
source /cvmfs/sw.hsf.org/key4hep/setup.sh -r 2026-04-08
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=install
cmake --build build --target install -j4
source setup.sh
```

The release `2026-04-08` is tested on Manivald and lxplus. Any Key4hep release
with Gaudi, k4FWCore, EDM4hep, podio, FastJet, ONNX Runtime, ROOT and a C++20
compiler should work.

**In every new shell**, set up the environment again before running:

```bash
cd k4mlTau
source /cvmfs/sw.hsf.org/key4hep/setup.sh -r 2026-04-08
source setup.sh
```

## Running

Process all EDM4hep files in a directory (here one output file per input file):

```bash
python3 scripts/run_mltau.py \
  --input-dir /path/to/edm4hep/root/files \
  --output-dir /path/to/output \
  --files-per-output 1
```

The same command runs on a batch system by adding `--workflow`:

```bash
# Slurm (e.g. Manivald)
python3 scripts/run_mltau.py --workflow slurm  --input-dir <dir> --output-dir <dir>

# HTCondor (e.g. lxplus)
python3 scripts/run_mltau.py --workflow condor --input-dir <dir> --output-dir <dir>
```

Add `--dry-run` to only print what would be run or submitted.

| Option | Meaning | Default |
| --- | --- | --- |
| `--input-dir` | directory with the input EDM4hep `.root` files | required |
| `--output-dir` | where the output `mltau_XXXXXX.root` files are written | `outputs` |
| `--workflow` | `local`, `slurm` or `condor` | `local` |
| `--files-per-output` | input files per output file (= per batch job) | `20` |
| `--max-files` | use only the first N input files (sorted), `-1` = all | `-1` |
| `--model-bundle` | model directory with `model.onnx` + `metadata.json` | the bundle in `models/` |

The input files are sorted and split into chunks; each chunk gives one output
file and, on a batch system, one job:

```text
<output_dir>/mltau_000000.root
<output_dir>/mltau_000001.root
...
```

One input file of 100 events takes from a few seconds up to ~30 s. Shared
defaults (model bundle, Key4hep release, Slurm/HTCondor job settings) are in
`config/main.toml`; normally there is no need to edit it.

Monitor batch jobs with `squeue -u $USER` (Slurm) or `condor_q` (HTCondor).
Slurm logs go to `<output_dir>/logs/`, HTCondor logs to `condor_logs/` in the
repository.

## Running on lxplus (HTCondor)

The steps are the same as above; these points are specific to lxplus.

**Where to put things**

- Code, build and model: **AFS** (your home `/afs/cern.ch/user/<x>/<user>/` or
  work area `/afs/cern.ch/work/<x>/<user>/`). k4mlTau needs only ~30 MB.
- Input and output ROOT files: **EOS** (`/eos/user/<x>/<user>/...`).
- HTCondor job logs cannot be on EOS. The runner therefore writes them, together
  with the generated submit file, to `condor_logs/` inside the repository (on
  AFS). Another location can be set with `log_dir` in the `[condor]` section of
  `config/main.toml`.

**Getting the model and input files onto lxplus**

The release is private, so download the model in your browser on your own
computer and copy it to lxplus:

```bash
# on lxplus, once: create the target folder
mkdir -p ~/k4mlTau/models

# on your own computer
scp ~/Downloads/mlTau_0612_multipartau_full_b8483f6.tar.gz* \
    <user>@lxplus.cern.ch:~/k4mlTau/models/
```

Then unpack it on lxplus as in [step 2](#2-download-and-unpack-the-model)
(starting from `cd models`). Input files can be copied to EOS the same way, e.g.
`scp *.root <user>@lxplus.cern.ch:/eos/user/<x>/<user>/inputs/`.

**Submitting**

```bash
python3 scripts/run_mltau.py --workflow condor \
  --input-dir /eos/user/<x>/<user>/inputs \
  --output-dir /eos/user/<x>/<user>/outputs \
  --files-per-output 10
condor_q
```

Each job must finish within the time limit of its job flavour, set by
`job_flavour` in `config/main.toml`: `espresso` (20 min, default),
`microcentury` (1 h), `longlunch` (2 h), `workday` (8 h). With ~30 s per input
file, `--files-per-output 10` fits comfortably into `espresso`. If a job fails,
look at `condor_logs/mltau.<cluster>.<job>.err`.

**Troubleshooting**

- `Could not chdir to home directory ... Connection timed out` or
  `no tokens?` after login: get Kerberos/AFS tokens with
  `kinit <user>@CERN.CH` and `aklog`, then `cd ~`.
- Messages that `PodioInput`/`PodioOutput` are deprecated are harmless.

## Checking the output

```bash
podio-dump <output_dir>/mltau_000000.root | grep -E "mlTau|mlTauID|mlTauSeedJets"
```

shows the three new collections (`mlTau`, `mlTauID`, `mlTauSeedJets`); see
[Output Schema](#output-schema) for their content.

## Running a single file with k4run

Without the runner, one input file can be processed directly:

```bash
MODEL_BUNDLE=$PWD/models/mlTau_0612_multipartau_full_b8483f6

MLTAU_INPUT=<INPUT_EDM4HEP_ROOT> \
MLTAU_OUTPUT=<OUTPUT_EDM4HEP_ROOT> \
MLTAU_MODEL=$MODEL_BUNDLE/model.onnx \
MLTAU_METADATA=$MODEL_BUNDLE/metadata.json \
MLTAU_INPUT_PARTICLES=PandoraPFOs \
k4run options/mlTau.py
```

## Configuration

Normal running is controlled by the command-line options of `scripts/run_mltau.py`
and the shared defaults in `config/main.toml`. The lower-level Gaudi options file also accepts
environment variables such as `MLTAU_INPUT`, `MLTAU_OUTPUT`, `MLTAU_MODEL`, and
`MLTAU_METADATA`; this is mainly useful for debugging or custom workflows.

Advanced environment variables include:

- `MLTAU_INPUT_PARTICLES`: input reconstructed particle collection, default
  `PandoraPFOs`.
- `MLTAU_INPUT_TRACKS`: track collection for lifetime/IP features, default
  `SiTracks_Refitted`.
- `MLTAU_INPUT_PRIMARY_VERTICES`: primary-vertex collection for lifetime/IP
  features, default `PrimaryVertices`.
- `MLTAU_TAU_SCORE_CUT`: minimum tau score written to output, default `0.0`.
- `MLTAU_OUTPUT_LEVEL`: `INFO` or `DEBUG`, default `INFO`.
- `MLTAU_INPUT_DUMP`: optional JSONL dump of ONNX input tensors for debugging.

## Model Metadata

The metadata file defines the input feature order, output mapping, decay-mode
mapping, and optional scaler parameters. Input scaling is applied only if the
metadata contains:

```json
"input_scaling": {
  "enabled": true,
  "feature_indices": [...],
  "mean": [...],
  "std": [...]
}
```

For models trained without input scaling, use metadata with
`"input_scaling.enabled": false`.

The ONNX model inputs are:

| Name | Shape | Meaning |
| --- | --- | --- |
| `cand_features` | `[batch, 17, 20]` | Candidate features in training order |
| `cand_kinematics` | `[batch, 4, 20]` | Candidate `[px, py, pz, energy]` |
| `cand_mask` | `[batch, 1, 20]` | Real candidate mask |

For `MultiParTau`, the ONNX outputs are:

| Name | Shape | Meaning |
| --- | --- | --- |
| `is_tau_logits` | `[batch, 2]` | Tau ID logits; tau score is softmax class 1 |
| `charge_logit` | `[batch]` | Positive-charge logit; score is sigmoid |
| `decay_mode_logits` | `[batch, 6]` | HPS decay-mode logits |
| `kinematics` | `[batch, 5]` | Visible tau p4 corrections |

Decay-mode class order:

```text
0 -> HPS 0
1 -> HPS 1
2 -> HPS 2
3 -> HPS 10
4 -> HPS 11
5 -> HPS 15 / rare / other
```

Kinematic decoding:

```text
pt_tau   = pt_seed * exp(output[0])
eta_tau  = eta_seed + output[1]
phi_tau  = phi_seed + atan2(output[2], output[3])
mass_tau = mass_seed * exp(output[4])
```

## Output Schema

`mlTau` is an `edm4hep::ReconstructedParticleCollection`:

- `mlTau.momentum`, `mlTau.energy`, `mlTau.mass`: regressed visible tau p4.
- `mlTau.charge`: predicted tau charge, `+1` or `-1`.
- `mlTau.PDG`: predicted tau PDG hypothesis.
- `mlTau.goodnessOfPID`: tau ID score.
- `mlTau.particles`: links to the PF constituents used as model inputs.

`mlTauID` is an `edm4hep::ParticleIDCollection` linked one-to-one to `mlTau`:

- `mlTauID.type`: predicted HPS decay mode.
- `mlTauID.PDG`: predicted tau PDG hypothesis.
- `mlTauID.likelihood`: tau ID score.
- `mlTauID.algorithmType`: fixed integer code identifying mlTau output.
- `mlTauID.parameters[0]`: positive-charge score, `P(charge = +1)`.
- `mlTauID.parameters[1]`: `P(DM = 0)`.
- `mlTauID.parameters[2]`: `P(DM = 1)`.
- `mlTauID.parameters[3]`: `P(DM = 2)`.
- `mlTauID.parameters[4]`: `P(DM = 10)`.
- `mlTauID.parameters[5]`: `P(DM = 11)`.
- `mlTauID.parameters[6]`: `P(DM = 15)`.

The field names are fixed by EDM4hep. The `mlTauID.parameters` layout is the
package convention for storing extra scores that do not have dedicated EDM4hep
fields.

## Notes

After moving the source tree or changing C++/CMake code, remove old CMake build
products before rebuilding:

```bash
rm -rf build build-gcc14 install setup.sh
```

`source setup.sh` is required after installation because it adds the locally
built Gaudi plugin and generated Python configurables to the runtime environment.
