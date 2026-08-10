# k4mlTau

`k4mlTau` is a Key4hep/FCCSW Gaudi package for running an exported ParTau ONNX
model and writing reconstructed tau objects to EDM4hep ROOT files.

The default reconstruction chain is:

```text
PandoraPFOs -> mlTauSeedBuilder -> mlTau -> mlTau (particles) + mlTauID (scores)
```

`mlTauSeedBuilder` clusters input reconstructed particles into tau seed jets.
`mlTau` builds the ParTau input tensors, applies optional input scaling from the
model metadata, runs ONNXRuntime, and writes tau candidates plus ID information.

## Quick Start

Clone the repository:

```bash
git clone git@github.com:nseeba/k4mlTau.git
cd k4mlTau
```

Download the model bundle from the repository releases:

```text
https://github.com/nseeba/k4mlTau/releases/tag/v0.1.0
```

Use the asset:

```text
mlTau_0612_multipartau_full_b8483f6.tar.gz
```

Unpack it under `models/`:

```bash
mkdir -p models
tar -xzf <PATH_TO_DOWNLOAD>/mlTau_0612_multipartau_full_b8483f6.tar.gz -C models
```

After unpacking, the model bundle should look like:

```text
models/
  mlTau_0612_multipartau_full_b8483f6/
    model.onnx
    metadata.json
```

The release also provides a `.sha256` checksum file. This is optional, but can
be used to verify the downloaded archive:

```bash
sha256sum -c mlTau_0612_multipartau_full_b8483f6.tar.gz.sha256
```

Build the Gaudi package. On Manivald, this is the tested Key4hep setup:

```bash
source /cvmfs/sw.hsf.org/key4hep/setup.sh -r 2026-04-08

cmake -B build-gcc14 -S . -DCMAKE_INSTALL_PREFIX=install
cmake --build build-gcc14 --target install -j4
source setup.sh
```

On another machine, use a Key4hep release that provides Gaudi, k4FWCore,
EDM4hep, podio, FastJet, ONNXRuntime, ROOT, and a C++20-capable compiler.

Run the default config locally:

```bash
python3 scripts/run_mltau.py
```

By default, the runner reads `config/main.toml`. The main fields are:

```toml
workflow = "local"      # "local" or "slurm"
input_dir = "/path/to/edm4hep/root/files"
output_dir = "outputs"
model_bundle = "models/mlTau_0612_multipartau_full_b8483f6"
max_files = 100         # -1 means all files
files_per_output = 20   # 20 input files -> one output ROOT file
input_particles = "PandoraPFOs"
key4hep_release = "2026-04-08"

[slurm]
job_name = "mltau"
```

The runner sorts the selected `.root` files, splits them in memory, and writes
one output file per chunk:

```text
<output_dir>/mltau_000000.root
<output_dir>/mltau_000001.root
...
```

Common config fields can be overridden on the command line:

```bash
python3 scripts/run_mltau.py \
  --workflow local \
  --input-dir /path/to/root/files \
  --output-dir outputs/test \
  --max-files 100 \
  --files-per-output 20
```

Submit the same workflow to Slurm:

```bash
python3 scripts/run_mltau.py --workflow slurm --dry-run
python3 scripts/run_mltau.py --workflow slurm
```

For a direct one-output Gaudi job, bypass the runner and call `k4run` directly:

```bash
MODEL_BUNDLE=$PWD/models/mlTau_0612_multipartau_full_b8483f6

MLTAU_INPUT=<INPUT_EDM4HEP_ROOT> \
MLTAU_OUTPUT=<OUTPUT_EDM4HEP_ROOT> \
MLTAU_MODEL=$MODEL_BUNDLE/model.onnx \
MLTAU_METADATA=$MODEL_BUNDLE/metadata.json \
MLTAU_INPUT_PARTICLES=PandoraPFOs \
k4run options/mlTau.py
```

Inspect the output collections:

```bash
podio-dump <OUTPUT_EDM4HEP_ROOT> | grep -E "mlTau|mlTauID|mlTauSeedJets"
rootls -t <OUTPUT_EDM4HEP_ROOT> | grep -E "mlTau|mlTauID|mlTauSeedJets"
```

## Configuration

Normal production running is controlled by `config/main.toml` and
`scripts/run_mltau.py`. The lower-level Gaudi options file also accepts
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
