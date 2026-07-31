# RT Phase 13 Gate: DLSS Ray Reconstruction

## Result

Phase 13 is complete for the interactive Vulkan path-traced preview. Official
signed NVIDIA Streamline 2.12.0 DLSS Ray Reconstruction executes on the target
RTX hardware, overrides separate SR/NRD processing while active, and preserves
temporal history through ordinary camera motion.

## Final input contract

- Low-resolution straight RGBA16F noisy HDR color and full-resolution RGBA16F
  output.
- R32F hardware depth and dedicated RG32F dense current-to-previous motion.
- Independent linear RGBA16F diffuse albedo, EnvBRDF specular albedo, and
  packed world-space shading-normal/linear-roughness guides.
- Dedicated R32F world-space specular hit distance measured from the primary
  surface, paired with `worldToCameraView` and `cameraViewToWorld`.
- Positive pixel-space Halton jitter is reported to Streamline; the unjittered
  inverse projection samples `pixelCenter-jitter`.
- The jitter phase count follows NVIDIA's
  `round(8 * (outputWidth / renderWidth)^2)` convention.

## Temporal and lifecycle contract

- Raw PT accumulation and Streamline reconstruction use separate history
  compatibility keys.
- Camera and deforming-object motion reset raw accumulation but remain valid
  RR history when dense motion is available.
- First frame, missing motion history, scene/material incompatibility,
  feature/output reconfiguration, resize/minimize, dialog suspension, and
  failed evaluation force RR history reset.
- A compatibility key is committed only after successful Streamline
  evaluation.
- Still Render remains raw, full resolution, and outside Streamline.

## Verification

- Serialized Debug builds passed for `xpbd_baker_app`,
  `xpbd_viewport_regression_tests`, and
  `xpbd_app_session_regression_tests`.
- Both maintained regression executables passed.
- Signed-hardware Quality evaluation passed at
  `393x386 -> 590x579`.
- Diagnostics showed the expected initial `sl_reset=1`; later frames with
  changing view matrices retained the same compatibility key and used
  `sl_reset=0`.
- Re-enabling exact R32F specular hit distance did not reproduce the former
  blue/cyan corruption.
- The user visually accepted both SR and RR and verified the runnable desktop
  package:
  `<Desktop>\XPBD_Baker_RT_Debug-20260731-003832`.
- Packaged Streamline, DLSS SR, and DLSS RR binaries passed Windows
  Authenticode validation as NVIDIA Corporation binaries.

The next stage removes NRD completely, then integrates Reflex and DLSS Frame
Generation through NVIDIA's official Vulkan Streamline contract.
