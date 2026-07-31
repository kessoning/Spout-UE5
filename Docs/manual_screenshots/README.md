# Manual screenshots

Drop the User Manual screenshots here, then rebuild the PDF.

## Workflow

1. Capture a screenshot listed in `../SCREENSHOT_CHECKLIST.md`.
2. Save it in this folder using the exact filename from the checklist, for example
   `SP-005_sender-viewport-graph.png`.
3. Rebuild the manual:

   ```
   python ../build_user_manual.py
   ```

   (Requires Python with `reportlab`: `pip install reportlab`.)

Each placeholder in the PDF whose file exists here is replaced automatically by the embedded
image with a numbered caption. Missing files keep their instruction placeholder, so you can add
screenshots incrementally and rebuild as you go.

## Expected filenames

```
SP-001_plugin-enabled.png
SP-002_rhi-directx12.png
SP-003_plugin-folder.png
SP-004_spout-dll-binaries.png
SP-017_add-sender-node.png
SP-005_sender-viewport-graph.png
SP-018_timer-setup.png
SP-006_sender-rendertarget-graph.png
SP-007_scenecapture-source.png
SP-008_rendertarget-settings.png
SP-009_receiver-graph.png
SP-010_material-texture-parameter.png
SP-011_external-app-receiving.png
SP-012_spout-info-debug.png
SP-013_log-loaded.png
SP-014_log-dll-missing.png
SP-015_cvar-console.png
SP-016_example-content.png
```

PNG or JPG both work. See `../SCREENSHOT_CHECKLIST.md` for what each image must show.
