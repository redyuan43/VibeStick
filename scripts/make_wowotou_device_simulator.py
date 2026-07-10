#!/usr/bin/env python3
"""Create a local device-flow simulator for Wowotou animation frames."""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
FRAME_ROOT = ROOT / ".tmp" / "wowotou-frames-transparent"
OUTPUT = FRAME_ROOT / "device-sim.html"
FPS = 10
EXCLUDED_ASSETS = {
    "cloudling-building",
    "cloudling-dozing-to-sleeping",
    "cloudling-idle-to-dozing",
    "cloudling-idle-to-sleeping",
    "cloudling-mini-alert",
    "cloudling-mini-enter-sleep",
    "cloudling-mini-happy",
    "cloudling-mini-idle",
    "cloudling-mini-peek",
    "cloudling-mini-sleep",
    "cloudling-mini-typing",
    "cloudling-sleeping",
}


def has_visible_pixels(path: Path) -> bool:
    with Image.open(path) as image:
        return image.convert("RGBA").getchannel("A").getbbox() is not None


def build_assets() -> tuple[list[dict[str, object]], list[str]]:
    assets: list[dict[str, object]] = []
    skipped: list[str] = []
    for asset_dir in sorted(path for path in FRAME_ROOT.iterdir() if path.is_dir()):
        if asset_dir.name in EXCLUDED_ASSETS:
            skipped.append(f"{asset_dir.name} (excluded)")
            continue
        frames = sorted(asset_dir.glob("frame_*.png"))
        visible_frames = [frame for frame in frames if has_visible_pixels(frame)]
        if not visible_frames:
            skipped.append(asset_dir.name)
            continue
        assets.append(
            {
                "name": asset_dir.name,
                "frames": [f"{asset_dir.name}/{frame.name}" for frame in visible_frames],
                "dropped": len(frames) - len(visible_frames),
            }
        )
    return assets, skipped


def main() -> None:
    assets, skipped = build_assets()
    if not assets:
        raise SystemExit("no visible assets found")

    payload = json.dumps({"assets": assets, "skipped": skipped, "fps": FPS}, ensure_ascii=True)
    OUTPUT.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Wowotou Device Simulator</title>
  <style>
    :root {{
      color-scheme: dark;
      --bg: #000;
      --fg: #f0f0fa;
      --muted: rgba(240, 240, 250, .62);
      --line: rgba(240, 240, 250, .28);
      --panel: rgba(240, 240, 250, .07);
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--fg);
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      letter-spacing: .04em;
    }}
    main {{
      min-height: 100vh;
      display: grid;
      grid-template-columns: minmax(260px, 360px) minmax(280px, 1fr);
      gap: 32px;
      align-items: center;
      padding: 36px;
    }}
    .stage {{
      display: grid;
      place-items: center;
      gap: 22px;
    }}
    .screen {{
      width: min(70vw, 420px);
      aspect-ratio: 1;
      display: grid;
      place-items: center;
      border: 1px solid var(--line);
      background: #050608;
    }}
    canvas {{
      width: min(62vw, 336px);
      height: min(62vw, 336px);
      image-rendering: pixelated;
    }}
    .controls {{
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      justify-content: center;
    }}
    button {{
      min-width: 92px;
      min-height: 40px;
      border: 1px solid var(--line);
      border-radius: 32px;
      background: var(--panel);
      color: var(--fg);
      font: inherit;
      font-size: 12px;
      letter-spacing: .1em;
      text-transform: uppercase;
      cursor: pointer;
    }}
    button:hover {{ border-color: var(--fg); }}
    .meta {{
      display: grid;
      gap: 18px;
      align-content: center;
    }}
    h1 {{
      margin: 0;
      font-size: clamp(28px, 5vw, 56px);
      line-height: 1;
      letter-spacing: .08em;
      text-transform: uppercase;
    }}
    .readout {{
      display: grid;
      gap: 9px;
      padding-top: 4px;
      color: var(--muted);
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 13px;
      letter-spacing: .02em;
    }}
    .asset-name {{
      color: var(--fg);
      font-size: 15px;
      overflow-wrap: anywhere;
    }}
    .rail {{
      display: grid;
      gap: 8px;
      max-height: 36vh;
      overflow: auto;
      padding-right: 8px;
    }}
    .row {{
      display: grid;
      grid-template-columns: 42px 1fr 54px;
      gap: 10px;
      align-items: center;
      padding: 8px 0;
      border-top: 1px solid rgba(240, 240, 250, .14);
      color: var(--muted);
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 12px;
    }}
    .row.active {{ color: var(--fg); }}
    .note {{
      color: var(--muted);
      font-size: 12px;
      line-height: 1.6;
      max-width: 52ch;
    }}
    @media (max-width: 760px) {{
      main {{ grid-template-columns: 1fr; padding: 24px; }}
      .screen {{ width: min(88vw, 420px); }}
      canvas {{ width: min(78vw, 336px); height: min(78vw, 336px); }}
    }}
  </style>
</head>
<body>
  <main>
    <section class="meta">
      <h1>ANI SIM</h1>
      <div class="readout">
        <div class="asset-name" id="assetName"></div>
        <div id="assetMeta"></div>
        <div id="frameMeta"></div>
      </div>
      <div class="rail" id="assetRail"></div>
      <p class="note" id="skipNote"></p>
    </section>
    <section class="stage">
      <div class="screen">
        <canvas id="canvas" width="112" height="112"></canvas>
      </div>
      <div class="controls">
        <button id="prevBtn">Prev</button>
        <button id="nextBtn">Next</button>
        <button id="pauseBtn">Pause</button>
      </div>
    </section>
  </main>
  <script>
    const data = {payload};
    const fps = data.fps;
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const assetName = document.getElementById('assetName');
    const assetMeta = document.getElementById('assetMeta');
    const frameMeta = document.getElementById('frameMeta');
    const rail = document.getElementById('assetRail');
    const skipNote = document.getElementById('skipNote');
    let assetIndex = 0;
    let frameIndex = 0;
    let paused = false;
    let last = 0;

    function drawRail() {{
      rail.innerHTML = '';
      data.assets.forEach((asset, index) => {{
        const row = document.createElement('div');
        row.className = `row${{index === assetIndex ? ' active' : ''}}`;
        row.innerHTML = `<span>${{String(index + 1).padStart(2, '0')}}</span><span>${{asset.name}}</span><span>${{asset.frames.length}}f</span>`;
        row.addEventListener('click', () => switchAsset(index));
        rail.appendChild(row);
      }});
      skipNote.textContent = data.skipped.length
        ? `Skipped empty assets: ${{data.skipped.join(', ')}}`
        : 'No empty assets skipped.';
    }}

    function switchAsset(nextIndex) {{
      assetIndex = (nextIndex + data.assets.length) % data.assets.length;
      frameIndex = 0;
      drawRail();
      drawFrame();
    }}

    function drawFrame() {{
      const asset = data.assets[assetIndex];
      const src = asset.frames[frameIndex % asset.frames.length];
      const image = new Image();
      image.onload = () => {{
        ctx.fillStyle = '#050608';
        ctx.fillRect(0, 0, 112, 112);
        ctx.drawImage(image, 0, 0);
        assetName.textContent = asset.name;
        assetMeta.textContent = `asset ${{assetIndex + 1}}/${{data.assets.length}} | frames ${{asset.frames.length}} | dropped transparent ${{asset.dropped}}`;
        frameMeta.textContent = `frame ${{frameIndex + 1}}/${{asset.frames.length}} | fps ${{fps}}`;
      }};
      image.src = src;
    }}

    function tick(time) {{
      if (!paused && time - last >= 1000 / fps) {{
        frameIndex = (frameIndex + 1) % data.assets[assetIndex].frames.length;
        drawFrame();
        last = time;
      }}
      requestAnimationFrame(tick);
    }}

    document.getElementById('nextBtn').addEventListener('click', () => switchAsset(assetIndex + 1));
    document.getElementById('prevBtn').addEventListener('click', () => switchAsset(assetIndex - 1));
    document.getElementById('pauseBtn').addEventListener('click', (event) => {{
      paused = !paused;
      event.currentTarget.textContent = paused ? 'Play' : 'Pause';
    }});
    window.addEventListener('keydown', (event) => {{
      if (event.code === 'Space' || event.code === 'ArrowRight') {{
        event.preventDefault();
        switchAsset(assetIndex + 1);
      }}
      if (event.code === 'ArrowLeft') {{
        event.preventDefault();
        switchAsset(assetIndex - 1);
      }}
    }});

    drawRail();
    drawFrame();
    requestAnimationFrame(tick);
  </script>
</body>
</html>
""",
        encoding="utf-8",
    )
    print(OUTPUT)
    print(f"assets={len(assets)} skipped={len(skipped)}")


if __name__ == "__main__":
    main()
