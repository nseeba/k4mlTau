# ParTau Gaudi Integration Notes

This package is the Gaudi-side implementation of ParTau inference for FCC
analysis workflows. It is intentionally separate from the training/export code:
the training repository produces an ONNX bundle, while this package consumes that
bundle in a Key4hep/FCCSW job.

## Inference Contract

Required files:

```text
<ONNX_BUNDLE>/model.onnx
<ONNX_BUNDLE>/metadata.json
```

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

## Gaudi Chain

The default algorithm chain is:

```text
PandoraPFOs -> mlTauSeedBuilder -> mlTau -> mlTau + mlTauID
```

`mlTauSeedBuilder` clusters reconstructed particles with FastJet
`ee_genkt_algorithm`, radius `0.4`, power `-1`, and writes seed jets with PF
constituent links.

`mlTau` reads seed jets and their constituents, builds the 17-feature tensor,
applies optional scaler parameters from `metadata.json`, runs ONNXRuntime, and
writes the output collections.

## Validation Strategy

For production integration, the key checks are that the ONNX bundle has been
validated in the training/export workflow and that the Gaudi output ROOT file
contains `mlTau`, `mlTauID`, and `mlTauSeedJets` with the expected EDM4hep types.
