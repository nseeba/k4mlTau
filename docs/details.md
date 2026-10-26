# k4mlTau – technical details

Reference for the output content, the options and the model interface. For
installing and running, see the [README](../README.md).

## Output collections

The output file contains the complete input events (all collections of the input
files) plus three new collections. The new collections have one entry per seed
jet, in the same order (as long as no positive `MLTAU_TAU_SCORE_CUT` is set).
Collections written with an older EDM4hep version are converted to the version of
the Key4hep release (podio schema evolution), as in any Key4hep job.

**`mlTauSeedJets`** (`edm4hep::ReconstructedParticleCollection`): the seed jets
(FastJet ee_genkt, anti-kt, R = 0.4, on `PandoraPFOs`).

**`mlTau`** (`edm4hep::ReconstructedParticleCollection`): the τh candidates

| Field | Content |
| --- | --- |
| `momentum`, `energy`, `mass` | regressed visible τh 4-momentum |
| `charge` | predicted charge, `+1` or `-1` |
| `PDG` | `-15` for charge +1, `15` for charge -1 |
| `goodnessOfPID` | τh ID score |
| `particles` | links to the particle-flow candidates used as model input |

**`mlTauID`** (`edm4hep::ParticleIDCollection`, one-to-one with `mlTau`)

| Field | Content |
| --- | --- |
| `type` | predicted decay mode (HPS code: 0, 1, 2, 10, 11, 15) |
| `PDG` | same as `mlTau.PDG` |
| `likelihood` | τh ID score |
| `algorithmType` | fixed code `15` identifying k4mlTau |
| `parameters[0]` | charge score `P(q = +1)` |
| `parameters[1..6]` | decay-mode probabilities `P(DM = 0, 1, 2, 10, 11, 15)` |

The field names are fixed by EDM4hep; the `parameters` layout is the k4mlTau
convention.

## Seed jets with an electron or muon

Seed jets with an electron or muon constituent (|PDG| = 11 or 13) were removed
from the training sample. By default the model is not run on them; they are
still written (so the collections stay one-to-one) with:

- score `-1` (`goodnessOfPID`, `likelihood`), decay mode `type = -1`, `PDG = 0`,
  all `parameters = -1`
- 4-momentum of the seed jet, charge of the highest-momentum electron/muon
  (`+1`/`-1`), links to all constituents of the seed jet

Any cut on the score removes them. To run the model on all jets:

| How you run | Switch |
| --- | --- |
| `scripts/run_mltau.py` | `--no-lepton-veto` (or `veto_lepton_jets = false` in `config/main.toml`) |
| `k4run options/mlTau.py` | `MLTAU_VETO_LEPTON_JETS=0` |
| Gaudi property | `VetoLeptonJets = False` |

## Runner options (`scripts/run_mltau.py`)

| Option | Meaning | Default |
| --- | --- | --- |
| `--input-dir` | directory with the input EDM4hep `.root` files | required |
| `--output-dir` | where the `mltau_XXXXXX.root` files are written | `outputs` |
| `--workflow` | `local`, `slurm` or `condor` | `local` |
| `--files-per-output` | input files per output file (= per batch job) | `20` |
| `--max-files` | use only the first N input files (sorted); `-1` = all | `-1` |
| `--model-bundle` | directory with `model.onnx` and `metadata.json` | the bundle in `models/` |
| `--no-lepton-veto` | also run the model on seed jets with an e/µ | off |
| `--dry-run` | only print what would be run or submitted | off |

Shared defaults (model bundle, Key4hep release, Slurm and HTCondor settings) are
in `config/main.toml`.

## Running one file with k4run

```bash
MODEL_BUNDLE=$PWD/models/mlTau_0612_multipartau_full_b8483f6
MLTAU_INPUT=<input.root> \
MLTAU_OUTPUT=<output.root> \
MLTAU_MODEL=$MODEL_BUNDLE/model.onnx \
MLTAU_METADATA=$MODEL_BUNDLE/metadata.json \
k4run options/mlTau.py
```

Further environment variables of `options/mlTau.py`:

| Variable | Meaning | Default |
| --- | --- | --- |
| `MLTAU_INPUT_PARTICLES` | input particle collection | `PandoraPFOs` |
| `MLTAU_INPUT_TRACKS` | tracks for the impact-parameter features | `SiTracks_Refitted` |
| `MLTAU_INPUT_PRIMARY_VERTICES` | primary vertices for the impact-parameter features | `PrimaryVertices` |
| `MLTAU_TAU_SCORE_CUT` | write only candidates with a score of at least this value | `0.0` |
| `MLTAU_VETO_LEPTON_JETS` | `1` or `0`, see above | `1` |
| `MLTAU_OUTPUT_LEVEL` | `INFO` or `DEBUG` | `INFO` |
| `MLTAU_INPUT_COLLECTIONS` | comma-separated list of input collections to read and write; must include the three inputs above | unset = all |
| `MLTAU_INPUT_DUMP` | write the ONNX input tensors to a JSONL file (debugging) | unset |

## Model interface

The model bundle contains `model.onnx` and `metadata.json`. The metadata defines
the input feature order, the output mapping, the decay-mode mapping and,
optionally, the input scaling (applied only if `input_scaling.enabled` is
`true`, with `feature_indices`, `mean` and `std`).

ONNX inputs (the first 20 constituents of each seed jet, in the order of the input particle collection):

| Name | Shape | Content |
| --- | --- | --- |
| `cand_features` | `[batch, 17, 20]` | constituent features in training order |
| `cand_kinematics` | `[batch, 4, 20]` | constituent `(px, py, pz, E)` |
| `cand_mask` | `[batch, 1, 20]` | 1 for real constituents, 0 for padding |

ONNX outputs (MultiParTau):

| Name | Shape | Content |
| --- | --- | --- |
| `is_tau_logits` | `[batch, 2]` | τh ID; score = softmax, class 1 |
| `charge_logit` | `[batch]` | charge; `P(q = +1)` = sigmoid |
| `decay_mode_logits` | `[batch, 6]` | decay mode, classes = HPS 0, 1, 2, 10, 11, 15 |
| `kinematics` | `[batch, 5]` | corrections to the seed-jet 4-momentum |

Kinematic decoding:

```text
pt   = pt_seed   * exp(out[0])
eta  = eta_seed  + out[1]
phi  = phi_seed  + atan2(out[2], out[3])
mass = mass_seed * exp(out[4])
```

For seed jets with (almost) zero mass, the mass correction amplifies numerical
noise; the mass of such candidates is not meaningful.

## Rebuilding from scratch

After moving the source tree or changing C++/CMake code, remove the old build
products and build again:

```bash
rm -rf build install setup.sh
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=install
cmake --build build --target install -j4
source setup.sh
```
