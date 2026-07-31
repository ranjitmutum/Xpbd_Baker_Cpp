# RT Phase 9 gate — LabPBR authoring, normal maps, and export

Status: **pass** (2026-07-30, Debug).

## User import contract

- Base color keeps its existing independent texture import.
- The LabPBR page exposes exactly two material attachment actions:
  `Import PBR PNG…` and `Import Normal PNG…`.
- Both actions select one image file directly. The UI has zero calls to the
  legacy suite/folder discovery APIs.
- The PBR map accepts RGB or RGBA PNG. RGB is a valid no-emission input:
  decoded alpha is treated as LabPBR's reserved `255` value and emission stays
  zero. RGBA retains the authored emission channel.
- The Iris normal map remains a read-only RGBA import so AO/depth alpha and
  exact source bytes survive removal/reimport/export.
- Legacy suite import remains only as a compatibility/test API; it is not a
  user-facing folder workflow.

## Authoring and export contract

- `GroupLabPbrOverride` supports draft/apply/revert and restore-from-texture.
- Group edits are isolated through rasterized UV coverage.
- A/R/G/B authoring covers emission, smoothness/roughness, metal/F0, and
  porosity/subsurface semantics.
- Conflicting values on overlapping group UV texels are reported
  deterministically and block export; conflicts are never silently resolved.
- Emission encodes over `[0,254]`; reserved `255` is never emitted.
- `_n` is copied from the original imported byte vector after checksum
  validation.
- `_s`, optional `_n`, and `texture.properties` are staged, round-trip
  validated, backed up, and installed as one recoverable export transaction.
- Existing targets require explicit overwrite confirmation.

## Automated evidence

Debug `xpbd_viewport_regression_tests`: pass.

- group isolation and mirrored-UV coverage;
- empty/untextured group safety;
- equal overlap accepted and unequal overlap blocked with exact claims;
- RGB specular no-emission semantics;
- emission maximum encodes to 254;
- Iris normal byte/checksum preservation and failed-import rollback;
- transactional bundle export and overwrite behavior;
- full exported-suite import, authored edit, overwrite export, and reimport;
- reimport preserves edited RGBA `_s`, byte-identical `_n`, and LabPBR 1.3
  properties.

Debug `xpbd_app_session_regression_tests`: pass.

- base color never discovers sibling PBR files;
- direct RGBA PBR and direct RGBA normal attachment;
- direct RGB PBR attachment with emission zero;
- same-size base reload preserves independently selected RGB/RGBA PBR and
  normal slots;
- failed direct import, suite replacement, relink, dirty draft, and cancelled
  overwrite preserve the committed session.

Debug `xpbd_baker_app`: build pass. English/Simplified Chinese locale parity:
**599/599**.

## Hardware evidence

Evidence directory: `.tmp/phase9-labpbr-20260730-1`.

Using the user's files directly and read-only:

- model: `<user-test-assets>\白水绘\models\main.json`;
- base: `<user-test-assets>\白水绘\textures\tex.png`;
- PBR: `<user-test-assets>\白水绘\textures\pbr\blue_s.png`;
- normal: `<user-test-assets>\白水绘\textures\pbr\bule_n.png`.

The first diagnostic exposed that `blue_s.png` is RGB. After enabling the
defined RGB/no-emission path, startup logs show both the PBR image and Iris
normal imported successfully as independent files. NVIDIA Vulkan PT completed
a 590x579, 32-sample capture with empty stderr. A second capture enabled the
procedural daytime sky so material response could be inspected under visible
environment lighting:
`baishui-direct-pbr-normal-daylight.png`.

No source model or texture file was modified.
