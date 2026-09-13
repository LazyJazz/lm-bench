#!/usr/bin/env python3
"""Build Sparkium CLI, render the benchmark scenes, and score the images."""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.color import deltaE_ciede2000, rgb2lab
from skimage.metrics import structural_similarity


EVAL_DIR = Path(__file__).resolve().parent
BENCH_ROOT = EVAL_DIR.parent
WORKSPACE = BENCH_ROOT / "workspace" / "LongMarch"
DATA_DIR = BENCH_ROOT / "test_files" / "data"
MANIFEST_PATH = DATA_DIR / "manifest.json"
BUILD_DIR = EVAL_DIR / ".build-longmarch"
RENDER_DIR = EVAL_DIR / "rendered"
RESULT_PATH = EVAL_DIR / "code_result.json"


def write_result(resolved: bool, score: float, reason: str) -> None:
    RESULT_PATH.parent.mkdir(parents=True, exist_ok=True)
    result = {
        "resolved": bool(resolved),
        "score": round(float(np.clip(score, 0.0, 1.0)), 6),
        "reason": str(reason),
    }
    temporary = RESULT_PATH.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(RESULT_PATH)


def run(command: list[str], *, timeout: int) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    return subprocess.run(
        command,
        cwd=BENCH_ROOT,
        env=os.environ.copy(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=True,
    )


def cuda_architecture() -> str | None:
    configured = os.environ.get("CMAKE_CUDA_ARCHITECTURES")
    if configured:
        return configured
    try:
        query = subprocess.run(
            ["nvidia-smi", "--query-gpu=compute_cap", "--format=csv,noheader"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=True,
        )
        capability = query.stdout.splitlines()[0].strip()
        if capability:
            return capability.replace(".", "")
    except (IndexError, OSError, subprocess.SubprocessError):
        pass
    return None


def configure_and_build() -> Path:
    if not (WORKSPACE / "CMakeLists.txt").is_file():
        raise RuntimeError(f"LongMarch workspace was not found at {WORKSPACE}")
    vcpkg = WORKSPACE.parent / "vcpkg"
    if not (vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
        raise RuntimeError(f"vcpkg was not found at {vcpkg}")

    configure = [
        "cmake",
        "-S",
        str(WORKSPACE),
        "-B",
        str(BUILD_DIR),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DVCPKG_PATH={vcpkg}",
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
    ]
    architecture = cuda_architecture()
    if architecture:
        configure.append(f"-DCMAKE_CUDA_ARCHITECTURES={architecture}")
    run(configure, timeout=1800)

    # Keep CI memory use predictable on machines that expose very large CPU counts.
    jobs = min(16, max(1, os.cpu_count() or 1))
    run(
        ["cmake", "--build", str(BUILD_DIR), "-j", str(jobs), "--target", "demo_sparkium_cpu_cli"],
        timeout=3600,
    )
    executable = BUILD_DIR / "demo" / "sparkium_cpu_cli" / "demo_sparkium_cpu_cli"
    if not executable.is_file():
        raise RuntimeError(f"build succeeded but CLI is missing: {executable}")
    return executable


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def srgb_to_linear(color: np.ndarray) -> np.ndarray:
    return np.where(
        color <= 0.04045,
        color / 12.92,
        ((color + 0.055) / 1.055) ** 2.4,
    )


def compare_images(candidate_path: Path, reference_path: Path) -> tuple[float, str]:
    candidate_rgb = load_rgb(candidate_path)
    reference_rgb = load_rgb(reference_path)
    if candidate_rgb.shape != reference_rgb.shape:
        return 0.0, f"dimension mismatch {candidate_rgb.shape} vs {reference_rgb.shape}"

    candidate_linear = srgb_to_linear(candidate_rgb)
    reference_linear = srgb_to_linear(reference_rgb)

    error = candidate_linear - reference_linear
    rmse = float(np.sqrt(np.mean(error * error)))
    rmse_score = max(0.0, 1.0 - rmse / 0.20)

    weights = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    candidate_luma = candidate_linear @ weights
    reference_luma = reference_linear @ weights
    ssim = float(
        structural_similarity(
            reference_luma,
            candidate_luma,
            data_range=1.0,
            gaussian_weights=True,
            sigma=1.5,
            use_sample_covariance=False,
        )
    )
    ssim = float(np.clip(ssim, 0.0, 1.0))

    delta_e = deltaE_ciede2000(rgb2lab(reference_rgb), rgb2lab(candidate_rgb))
    mean_delta_e = float(np.mean(delta_e))
    color_score = math.exp(-mean_delta_e / 20.0)

    score = 0.55 * ssim + 0.30 * rmse_score + 0.15 * color_score
    detail = (
        f"score={score:.4f}, SSIM={ssim:.4f}, linear-RMSE={rmse:.5f}, "
        f"mean-dE00={mean_delta_e:.3f}"
    )
    return float(np.clip(score, 0.0, 1.0)), detail


def main() -> int:
    try:
        manifest = json.loads(MANIFEST_PATH.read_text())
        scenes = manifest["scenes"]
        reference_spp = int(manifest["reference_spp"])
        render_spp = int(os.environ.get("SPARKIUM_EVAL_SPP", manifest["default_render_spp"]))
        threshold = float(os.environ.get("SPARKIUM_EVAL_THRESHOLD", manifest["pass_threshold"]))
        if not scenes or render_spp <= 0 or not 0.0 <= threshold <= 1.0:
            raise RuntimeError("invalid evaluation manifest or environment override")

        executable = configure_and_build()
        RENDER_DIR.mkdir(parents=True, exist_ok=True)
        scene_results: list[tuple[str, int, float, str]] = []
        scores: list[float] = []

        for entry in scenes:
            name = entry["name"]
            scene_path = DATA_DIR / entry["scene"]
            reference_path = DATA_DIR / entry["reference"]
            output_path = RENDER_DIR / f"{name}_{render_spp}spp.png"
            run(
                [
                    str(executable),
                    str(scene_path),
                    "--spp",
                    str(render_spp),
                    "--output",
                    str(output_path),
                ],
                timeout=900,
            )
            scene_score, detail = compare_images(output_path, reference_path)
            scores.append(scene_score)
            scene_results.append((name, render_spp, scene_score, detail))

        mean_score = float(np.mean(scores))
        minimum_score = float(np.min(scores))
        resolved = all(scene_score >= threshold for scene_score in scores)
        details = [
            f"{name}={'PASS' if scene_score >= threshold else 'FAIL'} "
            f"({actual_spp} vs {reference_spp} spp): {detail}"
            for name, actual_spp, scene_score, detail in scene_results
        ]
        reason = (
            f"every image must meet threshold={threshold:.3f}; "
            f"minimum={minimum_score:.4f}; mean={mean_score:.4f}; "
            + "; ".join(details)
        )
        write_result(resolved, minimum_score, reason)
        print(RESULT_PATH.read_text(), end="")
        return 0 if resolved else 1
    except Exception as error:
        output = getattr(error, "stdout", None)
        suffix = f"; command output: {output[-3000:]}" if output else ""
        write_result(False, 0.0, f"evaluation failed: {error}{suffix}")
        print(RESULT_PATH.read_text(), end="", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
