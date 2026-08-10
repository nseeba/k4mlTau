import os
from pathlib import Path

from Gaudi.Configuration import DEBUG, INFO, WARNING

from Configurables import ApplicationMgr, PodioInput, PodioOutput, k4DataSvc
from k4mlTau.mlTauPluginsConf import mlTau__mlTauAlg as mlTauAlg
from k4mlTau.mlTauPluginsConf import mlTau__mlTauSeedBuilder as mlTauSeedBuilder


def required_env(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def split_csv(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def read_input_list(path):
    with open(path, encoding="utf-8") as input_list:
        return [
            line.strip()
            for line in input_list
            if line.strip() and not line.lstrip().startswith("#")
        ]


def discover_input_files():
    input_list = os.environ.get("MLTAU_INPUT_LIST")
    input_dir = os.environ.get("MLTAU_INPUT_DIR")
    input_value = os.environ.get("MLTAU_INPUT")
    max_files = int(os.environ.get("MLTAU_MAX_FILES", "-1"))

    if input_list:
        files = read_input_list(input_list)
    elif input_dir:
        files = sorted(str(path) for path in Path(input_dir).glob("*.root"))
    elif input_value:
        entries = split_csv(input_value)
        if len(entries) == 1 and Path(entries[0]).is_dir():
            files = sorted(str(path) for path in Path(entries[0]).glob("*.root"))
        else:
            files = entries
    else:
        raise RuntimeError(
            "Set MLTAU_INPUT, MLTAU_INPUT_LIST, or MLTAU_INPUT_DIR."
        )

    if max_files >= 0:
        files = files[:max_files]

    if not files:
        raise RuntimeError("No input ROOT files selected.")

    missing = [path for path in files if not Path(path).is_file()]
    if missing:
        preview = ", ".join(missing[:5])
        suffix = " ..." if len(missing) > 5 else ""
        raise RuntimeError(f"Input files do not exist: {preview}{suffix}")

    return files


input_files = discover_input_files()
model_path = required_env("MLTAU_MODEL")
metadata_path = required_env("MLTAU_METADATA")

output_file = os.environ.get("MLTAU_OUTPUT", "mltau.root")
input_particles = os.environ.get("MLTAU_INPUT_PARTICLES", "PandoraPFOs")
input_tracks = os.environ.get("MLTAU_INPUT_TRACKS", "SiTracks_Refitted")
input_primary_vertices = os.environ.get("MLTAU_INPUT_PRIMARY_VERTICES", "PrimaryVertices")
seed_jets = os.environ.get("MLTAU_SEED_JETS", "mlTauSeedJets")
input_jets = os.environ.get("MLTAU_INPUT_JETS", seed_jets)
output_taus = os.environ.get("MLTAU_OUTPUT_TAUS", "mlTau")
output_tau_id = os.environ.get("MLTAU_OUTPUT_TAU_ID", "mlTauID")
tau_score_cut = float(os.environ.get("MLTAU_TAU_SCORE_CUT", "0.0"))
seed_radius = float(os.environ.get("MLTAU_SEED_RADIUS", "0.4"))
seed_genkt_power = float(os.environ.get("MLTAU_SEED_GENKT_POWER", "-1.0"))
seed_min_pt = float(os.environ.get("MLTAU_SEED_MIN_PT", "0.0"))
output_level_name = os.environ.get("MLTAU_OUTPUT_LEVEL", "INFO").upper()
output_level = DEBUG if output_level_name == "DEBUG" else INFO
input_collections = [
    item.strip()
    for item in os.environ.get(
        "MLTAU_INPUT_COLLECTIONS",
        f"{input_particles},{input_tracks},{input_primary_vertices}",
    ).split(",")
    if item.strip()
]

podio_event = k4DataSvc("EventDataSvc")
if len(input_files) == 1:
    podio_event.input = input_files[0]
else:
    podio_event.inputs = input_files

podio_input = PodioInput("PodioInput")
podio_input.OutputLevel = WARNING
podio_input.collections = input_collections

seed_builder = mlTauSeedBuilder("mlTauSeedBuilder")
seed_builder.OutputLevel = output_level
seed_builder.InputParticles = input_particles
seed_builder.OutputSeeds = seed_jets
seed_builder.Radius = seed_radius
seed_builder.GenKtPower = seed_genkt_power
seed_builder.MinPt = seed_min_pt

mltau = mlTauAlg("mlTau")
mltau.OutputLevel = output_level
mltau.ModelPath = model_path
mltau.MetadataPath = metadata_path
mltau.InputJets = input_jets
mltau.InputTracks = input_tracks
mltau.InputPrimaryVertices = input_primary_vertices
mltau.OutputTaus = output_taus
mltau.OutputTauID = output_tau_id
mltau.TauScoreCut = tau_score_cut
mltau.UseLifetimeVariables = os.environ.get("MLTAU_USE_LIFETIME", "1") not in (
    "0",
    "false",
    "False",
)
mltau.InputDumpPath = os.environ.get("MLTAU_INPUT_DUMP", "")

podio_output = PodioOutput("PodioOutput")
podio_output.OutputLevel = WARNING
podio_output.filename = output_file
podio_output.outputCommands = [
    "keep *",
]

ApplicationMgr(
    TopAlg=[podio_input, seed_builder, mltau, podio_output],
    ExtSvc=[podio_event],
    EvtSel="NONE",
    EvtMax=-1,
    OutputLevel=INFO,
)
