# Python Runtime Cleanup

## Target

Keep the Python package responsible only for battery telemetry on port 8878.
CapsWriter owns the production M5 voice bridge, device registry, OTA delivery,
recording sessions, and desktop interaction on port 8765.

## Sequence

1. Make the package entry point and installer telemetry-only.
2. Remove the legacy HUD installation and state-file contract.
3. Delete the unreachable legacy bridge, recording, provider, quota, and paste
   modules with their obsolete tests.
4. Preserve telemetry storage formats, CLI commands, dashboard assets, and
   firmware sample ingestion.

## Guardrails

- Do not delete telemetry raw journals or session history.
- Keep `vibestick-battery` and `vibestick-telemetry` entry points stable.
- Keep OTA publishing scripts; OTA serving is owned by CapsWriter.
- Verify telemetry tests before and after each deletion pass.
