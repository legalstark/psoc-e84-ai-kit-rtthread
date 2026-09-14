# AI Kit RT-Thread benchmark baselines

`pse84-ai-rtthread-v2.json` is the current complete 39/39 baseline for the PSoC Edge E84 AI Kit port. It defines status ABI 13 and benchmark protocol 7, including ResNet, MNIST, and Apache-2.0 Person Detection active/priority-peak suites. Older ABI/protocol baselines are intentionally not retained or accepted.

## Environment

- Board: KITPSE84AITOBO1 (PSoC Edge E84 AI Kit)
- Silicon reported by OpenOCD: PSE846GPS2DBZC4A, B0
- Probe: KitProg3 2.80.1529, SWD 12 MHz during programming
- Target voltage during programming: approximately 1.81 V
- Host IDE: RT-Thread Studio 2.3.0
- Source BSP: Edgi-Talk BSP 1.4.0 with the AI Kit board layer
- RT-Thread: 5.0.2
- Device Support/PDL: 1.1.1.824
- LVGL: 9.2
- Display: Waveshare 5-inch DSI LCD (B), 800x480
- Graphics: VG-Lite, double framebuffer, VSYNC
- M33 `SystemCoreClock`: 200 MHz
- M55 `SystemCoreClock`: 400 MHz
- Build profile: Release, `-O2`
- HMI and RT-Thread services remain active for system-active tests
- Peak NPU tests raise only the worker priority; they do not stop RT-Thread, interrupts, display, or touch

The U55 `kilocycles` values are raw Ethos-U PMU cycle measurements. Do not convert them to wall time using an assumed NPU frequency; compare the raw values between compatible reports.

## Artifacts

```text
Current combined release image
  file   ../../workspace/AI_Kit_RTThread_M55_Demo/build/ai_kit_rtthread_cm33_cm55.hex
  size   5012194 Bytes
  SHA256 FC5B7207CAD69E092475D1F054B5B06047614412500661C3F014222FC1B2F213

M33 ELF
  file   rt-thread.elf
  size   1697508 Bytes
  SHA256 2AFAD9765B4D1361C6EF767B3A519DB661431E7E288CDAF8915B3F262F0114B6

M55 image
  file   ../../workspace/AI_Kit_RTThread_M55_Demo/rtthread.hex
  size   4596112 Bytes
  SHA256 D4AB1A5FBAB2C3D2B3B5373D0EE210923FEE926B648C37FFF9FB960EEB7DF17A

Baseline JSON
  file   benchmarks/baselines/pse84-ai-rtthread-v2.json
  size   10091 Bytes
  SHA256 EC56457EB1096CE37C66FDD6C0D81879E98C1602CA830EDF2ADA13EFB680BBDA
```

The generated binaries are identified for reproducibility but remain build products. The JSON baseline is versioned source data.

## Reproduce and compare

Generate a new full candidate:

```powershell
.\scripts\capture_benchmark.ps1 -Port COM9 -RunAll
```

Compare it with the versioned baseline (the baseline path is the default):

```powershell
.\scripts\compare_benchmarks.ps1 `
    -Candidate .\artifacts\benchmarks\benchmark-<timestamp>.json
```

Exit code 0 means all default gated metrics are within the threshold. Exit code 2 means a performance regression exceeded the threshold. Input, schema, compatibility, or checksum failures use PowerShell's normal failure exit.

The default gate covers CPU/memory/IPC metrics and each priority-peak U55 PMU cycle metric. System-active NPU rows, priority-peak wall/throughput, and one-shot model-init/first-inference rows remain visible but informational because RT-Thread, interrupts, display, and touch stay active. Use `-GateVariable` only when deliberately treating those environment-sensitive rows as hard gates.

## Repeatability

Collect multiple reports under one directory, then summarize only the benchmark keys that are COMPLETE in every report:

```powershell
1..5 | ForEach-Object {
    .\scripts\capture_benchmark.ps1 -Port COM9 -RunNpu `
        -OutputDirectory ".\artifacts\repeatability\run$_"
}

.\scripts\analyze_benchmark_runs.ps1 `
    -Directory .\artifacts\repeatability `
    -OutputPath .\artifacts\repeatability\summary.json
```

The analyzer requires at least two validated reports with the same schema, ABI, protocol, catalog count, COMPLETE key set, benchmark names, and units. It reports mean, minimum, maximum, population standard deviation, and coefficient of variation (CV). `catalog_checksum` is deliberately not an identity field because it covers mutable result state and sequence values.

Repeatability output is descriptive evidence, not an automatic threshold update. Use priority-peak U55 PMU cycles to judge accelerator stability; active wall time and throughput include RT-Thread/LVGL scheduling effects.

## Updating the baseline

Do not overwrite `v2` after a toolchain, clock, model, benchmark algorithm, ABI, or protocol change. Create the next explicitly named baseline, remove the obsolete default, and update the comparison script only after:

1. a release build and `Verified OK` programming log;
2. one clean-boot `-RunAll` report with 39 COMPLETE results;
3. an immediate read-only capture with zero sequence changes;
4. independent JSON parsing and checksum validation;
5. the environment and image hashes are recorded here;
6. the change and measured differences are documented in `guide.md`.
