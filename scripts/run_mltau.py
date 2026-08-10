#!/usr/bin/env python3
"""Run k4mlTau locally or with Slurm using one config file."""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
from pathlib import Path


REPO_DIR = Path(os.environ.get("K4MLTAU_REPO_DIR", Path(__file__).resolve().parents[1])).resolve()


def parse_value(value: str):
    value = value.strip()
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        if not inner:
            return []
        return [parse_value(item.strip()) for item in inner.split(",")]
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    if value in {"true", "false"}:
        return value == "true"
    try:
        return int(value)
    except ValueError:
        return value


def load_config(path: str) -> dict:
    config: dict = {}
    section: dict | None = None
    with open(path, encoding="utf-8") as config_file:
        for raw_line in config_file:
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = config.setdefault(line[1:-1].strip(), {})
                continue
            if "=" not in line:
                raise SystemExit(f"Invalid config line: {raw_line.rstrip()}")
            key, value = line.split("=", 1)
            target = section if section is not None else config
            target[key.strip()] = parse_value(value)
    return config


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="config/main.toml", help="k4mlTau config file")
    parser.add_argument("--workflow", choices=("local", "slurm"), help="Override workflow in config")
    parser.add_argument("--input-dir", help="Override input_dir in config")
    parser.add_argument("--output-dir", help="Override output_dir in config")
    parser.add_argument("--max-files", type=int, help="Override max_files in config; -1 means all")
    parser.add_argument("--files-per-output", type=int, help="Override files_per_output in config")
    parser.add_argument("--model-bundle", help="Override model_bundle in config")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without running/submitting")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--chunk-index", type=int, help=argparse.SUPPRESS)
    return parser.parse_args()


def repo_path(value: str | os.PathLike[str]) -> Path:
    path = Path(value)
    return path if path.is_absolute() else REPO_DIR / path


def selected_files(config: dict) -> list[Path]:
    input_dir = Path(config["input_dir"])
    files = sorted(input_dir.glob("*.root"))
    max_files = int(config.get("max_files", -1))
    if max_files >= 0:
        files = files[:max_files]
    if not files:
        raise SystemExit(f"No .root files selected from {input_dir}")
    return files


def chunks(files: list[Path], files_per_output: int) -> list[list[Path]]:
    if files_per_output <= 0:
        raise SystemExit("files_per_output must be positive")
    return [files[i : i + files_per_output] for i in range(0, len(files), files_per_output)]


def chunk_output_path(config: dict, index: int) -> Path:
    output_dir = repo_path(config["output_dir"])
    return output_dir / f"mltau_{index:06d}.root"


def gaudi_command(config: dict, chunk: list[Path], output_path: Path) -> str:
    model_bundle = repo_path(config["model_bundle"])
    key4hep_release = config.get("key4hep_release", "2026-04-08")
    input_particles = config.get("input_particles", "PandoraPFOs")
    input_value = ",".join(str(path) for path in chunk)
    env = {
        "MLTAU_INPUT": input_value,
        "MLTAU_OUTPUT": str(output_path),
        "MLTAU_MODEL": str(model_bundle / "model.onnx"),
        "MLTAU_METADATA": str(model_bundle / "metadata.json"),
        "MLTAU_INPUT_PARTICLES": input_particles,
    }
    exports = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    return (
        "set -eo pipefail; "
        f"cd {shlex.quote(str(REPO_DIR))}; "
        "if ! command -v k4run >/dev/null 2>&1; then "
        f"source /cvmfs/sw.hsf.org/key4hep/setup.sh -r {shlex.quote(str(key4hep_release))}; "
        "fi; "
        "source setup.sh; "
        f"{exports} k4run options/mlTau.py"
    )


def run_chunk(config: dict, all_chunks: list[list[Path]], index: int, dry_run: bool) -> None:
    if index < 0 or index >= len(all_chunks):
        raise SystemExit(f"Chunk index {index} is out of range 0..{len(all_chunks) - 1}")
    output_path = chunk_output_path(config, index)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    command = gaudi_command(config, all_chunks[index], output_path)
    print(f"chunk {index + 1}/{len(all_chunks)}: {len(all_chunks[index])} input files -> {output_path}")
    if dry_run:
        print(command)
        return
    subprocess.run(["bash", "-c", command], check=True)


def run_local(config: dict, all_chunks: list[list[Path]], dry_run: bool) -> None:
    for index in range(len(all_chunks)):
        run_chunk(config, all_chunks, index, dry_run)


def submit_slurm(config_path: str, config: dict, n_chunks: int, dry_run: bool) -> None:
    slurm = config.get("slurm", {})
    job_name = slurm.get("job_name", "mltau")
    log_dir = repo_path(config["output_dir"]) / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    command = [
        "sbatch",
        f"--job-name={job_name}",
        f"--output={log_dir / (job_name + '_%A_%a.out')}",
        f"--array=0-{n_chunks - 1}",
        f"--export=ALL,K4MLTAU_REPO_DIR={REPO_DIR}",
    ]
    optional_slurm_args = {
        "partition": "--partition",
        "time": "--time",
        "mem": "--mem",
        "cpus_per_task": "--cpus-per-task",
    }
    for key, flag in optional_slurm_args.items():
        if key in slurm:
            command.append(f"{flag}={slurm[key]}")
    command.extend(slurm.get("extra_args", []))
    command.extend(
        [
            str(REPO_DIR / "scripts/run_mltau.py"),
            "--config",
            str(Path(config_path).resolve()),
            "--input-dir",
            str(config["input_dir"]),
            "--output-dir",
            str(config["output_dir"]),
            "--max-files",
            str(config.get("max_files", -1)),
            "--files-per-output",
            str(config["files_per_output"]),
            "--model-bundle",
            str(config["model_bundle"]),
            "--worker",
        ]
    )
    print(f"submitting {n_chunks} Slurm array tasks")
    print(" ".join(shlex.quote(part) for part in command))
    if dry_run:
        return
    subprocess.run(command, check=True)


def worker_index(args: argparse.Namespace) -> int:
    if args.chunk_index is not None:
        return args.chunk_index
    if "SLURM_ARRAY_TASK_ID" not in os.environ:
        raise SystemExit("Worker mode requires SLURM_ARRAY_TASK_ID or --chunk-index")
    return int(os.environ["SLURM_ARRAY_TASK_ID"])


def apply_overrides(config: dict, args: argparse.Namespace) -> None:
    overrides = {
        "workflow": args.workflow,
        "input_dir": args.input_dir,
        "output_dir": args.output_dir,
        "max_files": args.max_files,
        "files_per_output": args.files_per_output,
        "model_bundle": args.model_bundle,
    }
    for key, value in overrides.items():
        if value is not None:
            config[key] = value


def main() -> int:
    args = parse_args()
    config_path = repo_path(args.config)
    config = load_config(str(config_path))
    apply_overrides(config, args)
    workflow = config.get("workflow", "local")
    files = selected_files(config)
    all_chunks = chunks(files, int(config["files_per_output"]))
    print(f"selected files: {len(files)}")
    print(f"files per output: {config['files_per_output']}")
    print(f"outputs/tasks: {len(all_chunks)}")

    if args.worker:
        run_chunk(config, all_chunks, worker_index(args), args.dry_run)
    elif workflow == "local":
        run_local(config, all_chunks, args.dry_run)
    elif workflow == "slurm":
        submit_slurm(str(config_path), config, len(all_chunks), args.dry_run)
    else:
        raise SystemExit(f"Unknown workflow: {workflow}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
