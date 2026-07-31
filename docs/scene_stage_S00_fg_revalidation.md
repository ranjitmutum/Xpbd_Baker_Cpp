# S00 Frame Generation Revalidation

## Status

- Result: `InProgress`
- Input baseline: `664b4308fa5879ba83ad1523262cc1bc3df6f7ac`
- Final result commit: `Pending`
- GPU / driver / Windows / Vulkan / Streamline: `Pending final run`
- Gate authority: `assets/scene_tests/s00/s00_gate_spec.json`

FG remains default Off until this document is completed. Historical evidence is retained as input only and cannot substitute for a run on the final clean S00 result commit.

## Historical Inputs

| Evidence | Historical result | Use in S00 |
|---|---|---|
| `PERF15_raw_fg_300` | 298 samples / 268 steady / 0 parser errors | threshold context only |
| `PERF15_sr_fg_300` | 298 / 268 / 0 | threshold context only |
| `PERF15_rr_fg_300` | 298 / 268 / 0 | threshold context only |
| `FG_final_review_90` | 88 valid FG samples; proxy present-fence lifecycle disabled | lifecycle regression context only |

These runs predate the final S00 commit, do not embed a complete command/commit binding, and do not prove real Resize/Minimize/F11/Dialog, forced Native fallback, or rotating-object image correctness.

## Fixed Cases

| Case | Required outcome | Result | Evidence |
|---|---|---|---|
| Default startup | FG Off, stable Native present | Pending | Pending |
| Raw + FG | supported/active or explicit capability result; stable frames | Pending | Pending |
| SR Quality + FG | valid inputs/tags, stable frames | Pending | Pending |
| RR + FG | valid inputs/tags, stable frames | Pending | Pending |
| FG On/Off cycle | no stale proxy/resources/fences | Pending | Pending |
| Resize | recreate/continue, no device loss | Pending | Pending |
| Minimize/restore | suspend/resume cleanly | Pending | Pending |
| F11 | fullscreen transition cleanly | Pending | Pending |
| Dialog open/close | no present lifecycle corruption | Pending | Pending |
| Proxy fence ownership | application present-fence count `0` | Pending | Pending |
| Forced FG failure | fallback Native and continue Present | Pending | Pending |
| S00-B RR+FG rotation | one continuous silhouette, image thresholds pass | Pending | Pending |
| S00-B SR+FG rotation | one continuous silhouette, image thresholds pass | Pending | Pending |

## Command

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tools\scene_tests\run_s00_hardware_gate.ps1 `
  -App .\out\build\s00-gate\Release\xpbd_baker_app.exe `
  -Spec assets\scene_tests\s00\s00_gate_spec.json `
  -EvidenceRoot artifacts\scene_stage_S00 `
  -Cases fg_lifecycle,s00_b_rotating_fg
```

Expected exit code is `0`. The runner must enforce a wall-clock deadline by exact PID, save raw logs/AOVs/presented frames, and record all environment variables. Ambient FG/SR/RR variables must be cleared before each case.

## Acceptance

- No crash, hang, device loss, validation error, application-owned Present fence on a proxy swapchain, or abnormal shutdown.
- SR/RR input resources, extents, jitter, motion scale/direction, depth convention and reset tags match the rendered real frame.
- Rotating-object metrics satisfy the versioned S00 spec; a lifecycle-only success cannot waive a visible double ghost.
- An unsupported or failed FG capability is recorded explicitly and defaults Off. Such a capability result does not fail later scene stages, but it cannot be reported as FG Passed.

## Final Evidence

Pending the final clean S00 result commit and hardware execution.
