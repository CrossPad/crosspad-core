#!/usr/bin/env python3
"""
Auto-generate internal architecture diagrams for crosspad-core.

Analyzes the module structure (src/ subdirectories), #include relationships,
and interface/implementation organization to produce dependency diagrams.

Outputs Mermaid markdown, Graphviz DOT, optional SVG, and a self-contained HTML viewer.

Usage:
    python generate_diagrams.py [--project-root DIR] [--output-dir DIR]
                                [--no-html] [--no-dot] [--verbose]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# ────────────────────────── Configuration ──────────────────────────

MODULE_COLORS = {
    "app":       {"fill": "#0f2618", "stroke": "#238636", "text": "#7ee787"},
    "event":     {"fill": "#0d2039", "stroke": "#1f6feb", "text": "#79c0ff"},
    "pad":       {"fill": "#152210", "stroke": "#3fb950", "text": "#7ee787"},
    "led":       {"fill": "#152210", "stroke": "#3fb950", "text": "#7ee787"},
    "protocol":  {"fill": "#250d1a", "stroke": "#f85149", "text": "#ff7b72"},
    "stm32":     {"fill": "#250d1a", "stroke": "#f85149", "text": "#ff7b72"},
    "settings":  {"fill": "#261e0d", "stroke": "#d29922", "text": "#e3b341"},
    "midi":      {"fill": "#1c1a26", "stroke": "#6e40c9", "text": "#d2a8ff"},
    "music":     {"fill": "#1c1a26", "stroke": "#6e40c9", "text": "#d2a8ff"},
    "platform":  {"fill": "#1f0d2a", "stroke": "#8957e5", "text": "#d2a8ff"},
    "kit":       {"fill": "#261e0d", "stroke": "#d29922", "text": "#e3b341"},
    "sequencer": {"fill": "#1f0d2a", "stroke": "#8957e5", "text": "#d2a8ff"},
    "synth":     {"fill": "#1f0d2a", "stroke": "#8957e5", "text": "#d2a8ff"},
    "status":    {"fill": "#261e0d", "stroke": "#d29922", "text": "#e3b341"},
}

DEFAULT_COLOR = {"fill": "#1a2332", "stroke": "#3d6699", "text": "#79c0ff"}

LAYER_MAP = {
    "app":       "Application",
    "event":     "Infrastructure",
    "pad":       "Pad System",
    "led":       "Pad System",
    "protocol":  "STM32 Protocol",
    "stm32":     "STM32 Protocol",
    "settings":  "Configuration",
    "midi":      "MIDI",
    "music":     "MIDI",
    "platform":  "Interfaces",
    "kit":       "Interfaces",
    "sequencer": "Interfaces",
    "synth":     "Interfaces",
    "status":    "Interfaces",
}

LAYER_ORDER = ["Application", "Pad System", "MIDI", "STM32 Protocol", "Configuration", "Infrastructure", "Interfaces"]

LAYER_COLORS = {
    "Application":    {"fill": "#0f2618", "stroke": "#238636", "text": "#7ee787"},
    "Pad System":     {"fill": "#152210", "stroke": "#3fb950", "text": "#7ee787"},
    "MIDI":           {"fill": "#1c1a26", "stroke": "#6e40c9", "text": "#d2a8ff"},
    "STM32 Protocol": {"fill": "#250d1a", "stroke": "#f85149", "text": "#ff7b72"},
    "Configuration":  {"fill": "#261e0d", "stroke": "#d29922", "text": "#e3b341"},
    "Infrastructure": {"fill": "#0d2039", "stroke": "#1f6feb", "text": "#79c0ff"},
    "Interfaces":     {"fill": "#1f0d2a", "stroke": "#8957e5", "text": "#d2a8ff"},
}


# ────────────────────────── Data Model ──────────────────────────

@dataclass
class ModuleInfo:
    name: str
    src_files: List[str] = field(default_factory=list)
    header_files: List[str] = field(default_factory=list)
    interfaces: List[str] = field(default_factory=list)  # I*.hpp files
    layer: str = ""


# ────────────────────────── Discovery ──────────────────────────

def discover_modules(root: str, verbose: bool = False) -> Dict[str, ModuleInfo]:
    """Discover modules from src/ and include/ subdirectories."""
    modules: Dict[str, ModuleInfo] = {}

    src_dir = os.path.join(root, "src")
    inc_dir = os.path.join(root, "include", "crosspad")

    # Discover from include/ (may have modules with no src/)
    if os.path.isdir(inc_dir):
        for entry in os.scandir(inc_dir):
            if entry.is_dir():
                mod_name = entry.name
                mod = modules.setdefault(mod_name, ModuleInfo(name=mod_name))
                mod.layer = LAYER_MAP.get(mod_name, "Interfaces")
                for fn in os.listdir(entry.path):
                    if fn.endswith(('.h', '.hpp', '.hxx')):
                        mod.header_files.append(os.path.join(entry.path, fn))
                        if fn.startswith('I') and fn[1:2].isupper():
                            mod.interfaces.append(fn)

    # Discover from src/
    if os.path.isdir(src_dir):
        for entry in os.scandir(src_dir):
            if entry.is_dir():
                mod_name = entry.name
                mod = modules.setdefault(mod_name, ModuleInfo(name=mod_name))
                if not mod.layer:
                    mod.layer = LAYER_MAP.get(mod_name, "Interfaces")
                for dirpath, _, filenames in os.walk(entry.path):
                    for fn in filenames:
                        if fn.endswith(('.c', '.cpp', '.cxx')):
                            mod.src_files.append(os.path.join(dirpath, fn))

    if verbose:
        print(f"Discovered {len(modules)} modules:")
        for m in sorted(modules.values(), key=lambda x: x.name):
            print(f"  {m.name}: {len(m.src_files)} src, {len(m.header_files)} headers, "
                  f"{len(m.interfaces)} interfaces, layer={m.layer}")

    return modules


# ────────────────────────── Include Analysis ──────────────────────────

def parse_includes(file_path: str) -> List[str]:
    """Extract #include "..." paths from a source file."""
    includes = []
    try:
        with open(file_path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
                if m:
                    includes.append(m.group(1).replace('\\', '/'))
    except OSError:
        pass
    return includes


def resolve_module(include_path: str) -> Optional[str]:
    """Resolve an include path like 'crosspad/pad/PadManager.hpp' to module name 'pad'."""
    parts = include_path.split('/')
    # crosspad/<module>/...
    if len(parts) >= 2 and parts[0] == "crosspad":
        return parts[1]
    return None


def build_module_graph(
    modules: Dict[str, ModuleInfo],
    verbose: bool = False,
) -> Dict[Tuple[str, str], int]:
    """Build module-to-module dependency edges from #include directives."""
    edges: Dict[Tuple[str, str], int] = {}

    for mod in modules.values():
        all_files = mod.src_files + mod.header_files
        for fpath in all_files:
            includes = parse_includes(fpath)
            for inc in includes:
                target = resolve_module(inc)
                if target and target != mod.name and target in modules:
                    edge = (mod.name, target)
                    edges[edge] = edges.get(edge, 0) + 1

    if verbose:
        print(f"\nModule dependency edges: {len(edges)}")
        for (src, dst), count in sorted(edges.items()):
            print(f"  {src} -> {dst} ({count} includes)")

    return edges


# ────────────────────────── Mermaid Helpers ──────────────────────────

def mermaid_id(name: str) -> str:
    return name.replace("-", "_").replace(".", "_")


def mermaid_node(name: str) -> str:
    mid = mermaid_id(name)
    if mid != name:
        return f'{mid}["{name}"]'
    return mid


# ────────────────────────── Diagram Generators ──────────────────────────

def generate_module_deps_mermaid(
    modules: Dict[str, ModuleInfo],
    edges: Dict[Tuple[str, str], int],
) -> str:
    """Generate module dependency Mermaid graph."""
    lines = ["graph LR"]

    # Group by layer
    layers: Dict[str, List[str]] = {}
    for mod in modules.values():
        layers.setdefault(mod.layer, []).append(mod.name)

    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe = layer.replace(" ", "_")
        lines.append(f'    subgraph {safe}["{layer}"]')
        for name in sorted(layers[layer]):
            lines.append(f'        {mermaid_node(name)}')
        lines.append("    end")

    # Edges
    for (src, dst), count in sorted(edges.items()):
        label = f"|{count}|" if count > 1 else ""
        lines.append(f"    {mermaid_id(src)} -->{label} {mermaid_id(dst)}")

    # Styles
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe = layer.replace(" ", "_")
        c = LAYER_COLORS.get(layer, DEFAULT_COLOR)
        lines.append(f'    style {safe} fill:{c["fill"]},stroke:{c["stroke"]},color:{c["text"]}')

    return "\n".join(lines)


def generate_interface_map_mermaid(modules: Dict[str, ModuleInfo]) -> str:
    """Generate interface/implementation overview Mermaid graph."""
    lines = ["graph TB"]

    # Only modules with interfaces or src
    active = {n: m for n, m in modules.items() if m.interfaces or m.src_files}

    # Group: interfaces-only vs has-implementation
    lines.append('    subgraph Interfaces["Pure Interfaces (no src/)"]')
    for name in sorted(active):
        mod = active[name]
        if mod.interfaces and not mod.src_files:
            ifaces = "<br/>".join(f"&bull; {i}" for i in sorted(mod.interfaces))
            lines.append(f'        {mermaid_id(name)}["{name}<br/>{ifaces}"]')
    lines.append("    end")

    lines.append('    subgraph Implementations["Modules with Implementation"]')
    for name in sorted(active):
        mod = active[name]
        if mod.src_files:
            detail_parts = []
            if mod.interfaces:
                detail_parts.append(f"{len(mod.interfaces)} interfaces")
            detail_parts.append(f"{len(mod.src_files)} src files")
            detail = ", ".join(detail_parts)
            lines.append(f'        {mermaid_id(name)}["{name}<br/>{detail}"]')
    lines.append("    end")

    lines.append('    style Interfaces fill:#1f0d2a,stroke:#8957e5,color:#d2a8ff')
    lines.append('    style Implementations fill:#0d2039,stroke:#1f6feb,color:#79c0ff')

    return "\n".join(lines)


def generate_architecture_layers_mermaid(modules: Dict[str, ModuleInfo]) -> str:
    """Generate architecture layer Mermaid graph."""
    lines = ["graph TB"]

    layers: Dict[str, List[str]] = {}
    for mod in modules.values():
        layers.setdefault(mod.layer, []).append(mod.name)

    prev_id = None
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe = layer.replace(" ", "_")
        lines.append(f'    subgraph {safe}["{layer}"]')
        lines.append("        direction LR")
        for name in sorted(layers[layer]):
            mod = modules[name]
            src_count = len(mod.src_files)
            hdr_count = len(mod.header_files)
            lines.append(f'        {mermaid_id(name)}["{name}<br/>{src_count} src, {hdr_count} hdr"]')
        lines.append("    end")
        if prev_id is not None:
            lines.append(f"    {prev_id} --> {safe}")
        prev_id = safe

    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe = layer.replace(" ", "_")
        c = LAYER_COLORS.get(layer, DEFAULT_COLOR)
        lines.append(f'    style {safe} fill:{c["fill"]},stroke:{c["stroke"]},color:{c["text"]}')

    return "\n".join(lines)


# ────────────────────────── DOT Generators ──────────────────────────

def _dot_escape(name: str) -> str:
    return name.replace("-", "_").replace(".", "_")


def generate_module_deps_dot(
    modules: Dict[str, ModuleInfo],
    edges: Dict[Tuple[str, str], int],
) -> str:
    lines = [
        'digraph module_dependencies {',
        '    rankdir=LR;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '    edge [color="#58a6ff", fontname="Segoe UI", fontsize=9, fontcolor="#8b949e"];',
        '',
    ]

    layers: Dict[str, List[str]] = {}
    for mod in modules.values():
        layers.setdefault(mod.layer, []).append(mod.name)

    idx = 0
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        c = LAYER_COLORS.get(layer, DEFAULT_COLOR)
        lines.append(f'    subgraph cluster_{idx} {{')
        lines.append(f'        label="{layer}";')
        lines.append(f'        style=filled; fillcolor="{c["fill"]}"; color="{c["stroke"]}";')
        lines.append(f'        fontcolor="{c["text"]}"; fontname="Segoe UI"; fontsize=12;')
        for name in sorted(layers[layer]):
            lines.append(f'        {_dot_escape(name)} [label="{name}", fillcolor="{c["fill"]}", color="{c["stroke"]}"];')
        lines.append('    }')
        idx += 1

    lines.append('')
    for (src, dst), count in sorted(edges.items()):
        label = f' [label="{count}"]' if count > 1 else ''
        lines.append(f'    {_dot_escape(src)} -> {_dot_escape(dst)}{label};')

    lines.append('}')
    return '\n'.join(lines)


def generate_architecture_layers_dot(modules: Dict[str, ModuleInfo]) -> str:
    lines = [
        'digraph architecture_layers {',
        '    rankdir=TB;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '    edge [color="#58a6ff", fontname="Segoe UI", fontsize=9];',
        '',
    ]

    layers: Dict[str, List[str]] = {}
    for mod in modules.values():
        layers.setdefault(mod.layer, []).append(mod.name)

    idx = 0
    anchors = []
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        c = LAYER_COLORS.get(layer, DEFAULT_COLOR)
        anchor = f"_anchor_{idx}"
        lines.append(f'    subgraph cluster_{idx} {{')
        lines.append(f'        label="{layer}";')
        lines.append(f'        style=filled; fillcolor="{c["fill"]}"; color="{c["stroke"]}";')
        lines.append(f'        fontcolor="{c["text"]}"; fontname="Segoe UI"; fontsize=13;')
        for name in sorted(layers[layer]):
            lines.append(f'        {_dot_escape(name)} [label="{name}", fillcolor="{c["fill"]}", color="{c["stroke"]}"];')
        lines.append(f'        {anchor} [label="", shape=point, width=0, height=0];')
        lines.append('    }')
        anchors.append(anchor)
        idx += 1

    lines.append('')
    for i in range(len(anchors) - 1):
        lines.append(f'    {anchors[i]} -> {anchors[i + 1]} [style=bold, color="#58a6ff"];')

    lines.append('}')
    return '\n'.join(lines)


def generate_interface_map_dot(modules: Dict[str, ModuleInfo]) -> str:
    lines = [
        'digraph interface_map {',
        '    rankdir=TB;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '',
    ]

    lines.append('    subgraph cluster_interfaces {')
    lines.append('        label="Pure Interfaces"; style=filled; fillcolor="#1f0d2a"; color="#8957e5"; fontcolor="#d2a8ff";')
    for name, mod in sorted(modules.items()):
        if mod.interfaces and not mod.src_files:
            ifaces = "\\n".join(mod.interfaces)
            lines.append(f'        {_dot_escape(name)} [label="{name}\\n{ifaces}", fillcolor="#1f0d2a", color="#8957e5"];')
    lines.append('    }')

    lines.append('    subgraph cluster_impl {')
    lines.append('        label="Implementations"; style=filled; fillcolor="#0d2039"; color="#1f6feb"; fontcolor="#79c0ff";')
    for name, mod in sorted(modules.items()):
        if mod.src_files:
            lines.append(f'        {_dot_escape(name)} [label="{name}\\n{len(mod.src_files)} src", fillcolor="#0d2039", color="#1f6feb"];')
    lines.append('    }')

    lines.append('}')
    return '\n'.join(lines)


# ────────────────────────── HTML Generator ──────────────────────────

def generate_html(diagrams: Dict[str, str], timestamp: str, project_name: str) -> str:
    tab_defs = [
        ("Module Dependencies", "module_dependencies", "tag-yellow", "dependencies",
         "Internal module-to-module dependencies parsed from #include directives. Edge labels show include count."),
        ("Interface Map", "interface_map", "tag-purple", "interfaces",
         "Overview of pure interface modules (headers only) vs modules with implementation."),
        ("Architecture Layers", "architecture_layers", "tag-blue", "layers",
         "Layered view of the library's internal module organization."),
    ]

    tabs_html = ""
    panels_html = ""

    for idx, (title, key, tag_class, tag_text, desc) in enumerate(tab_defs):
        active = " active" if idx == 0 else ""
        tabs_html += f'  <div class="tab{active}" data-idx="{idx}">{title}</div>\n'

        mermaid_code = diagrams.get(key, "graph LR\n    A-->B")
        panels_html += f'''<div class="panel">
  <div class="card">
    <div class="card-header">
      <h2>{title}</h2>
      <span class="tag {tag_class}">{tag_text}</span>
    </div>
    <div class="card-body">
      <p style="font-size:0.78rem;color:var(--fg2);margin-bottom:12px">{desc}</p>
      <div class="mermaid-wrap">
        <div class="mermaid">
{mermaid_code}
        </div>
      </div>
    </div>
  </div>
</div>
'''

    return _html_template(project_name, timestamp, tabs_html, panels_html)


def _html_template(project_name: str, timestamp: str, tabs_html: str, panels_html: str) -> str:
    return f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{project_name} Auto-Generated Diagrams</title>
  <script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
  <style>
    :root {{
      --bg: #0d1117; --bg2: #161b22; --bg3: #1c2128;
      --border: #30363d; --border2: #21262d;
      --fg: #e6edf3; --fg2: #8b949e;
      --blue: #58a6ff; --green: #3fb950; --yellow: #d29922;
      --red: #f85149; --purple: #bc8cff;
      --radius: 8px;
    }}
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{ font-family: -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; background: var(--bg); color: var(--fg); }}
    header {{
      background: var(--bg2); border-bottom: 1px solid var(--border);
      padding: 14px 28px; display: flex; align-items: center; gap: 14px;
      position: sticky; top: 0; z-index: 100;
    }}
    header h1 {{ font-size: 1.15rem; font-weight: 600; }}
    header h1 span {{ color: var(--blue); }}
    .header-badges {{ display: flex; gap: 8px; margin-left: auto; }}
    .badge {{
      font-size: 0.7rem; padding: 3px 8px; border-radius: 12px;
      border: 1px solid var(--border); color: var(--fg2); background: var(--bg);
    }}
    .badge-auto {{ border-color: var(--green); color: var(--green); }}
    .tabs {{
      display: flex; gap: 2px; padding: 0 28px;
      border-bottom: 1px solid var(--border); background: var(--bg);
      overflow-x: auto; scrollbar-width: thin;
    }}
    .tab {{
      padding: 11px 18px; cursor: pointer; font-size: 0.82rem; font-weight: 500;
      color: var(--fg2); border-bottom: 2px solid transparent;
      transition: all 0.12s; user-select: none; white-space: nowrap;
    }}
    .tab:hover {{ color: var(--fg); }}
    .tab.active {{ color: var(--blue); border-bottom-color: var(--blue); }}
    .panel {{ padding: 28px; max-width: 1600px; margin: 0 auto; display: flex; flex-direction: column; gap: 20px; }}
    .panel.hidden {{ display: none; }}
    .card {{
      background: var(--bg2); border: 1px solid var(--border);
      border-radius: var(--radius); overflow: hidden;
    }}
    .card-header {{
      padding: 14px 20px; border-bottom: 1px solid var(--border);
      display: flex; align-items: center; gap: 10px;
    }}
    .card-header h2 {{ font-size: 0.82rem; font-weight: 600; color: var(--fg); }}
    .card-header .tag {{
      font-size: 0.65rem; padding: 2px 7px; border-radius: 10px; font-weight: 500; border: 1px solid;
    }}
    .tag-blue {{ color: var(--blue); border-color: #1f6feb55; background: #1f6feb15; }}
    .tag-green {{ color: var(--green); border-color: #3fb95055; background: #3fb95015; }}
    .tag-purple {{ color: var(--purple); border-color: #8957e555; background: #8957e515; }}
    .tag-yellow {{ color: var(--yellow); border-color: #d2992255; background: #d2992215; }}
    .card-body {{ padding: 20px; }}
    .mermaid-wrap {{
      overflow: hidden; height: 70vh; min-height: 400px; position: relative; cursor: grab;
      border-radius: 4px; background: #0d111799;
    }}
    .mermaid-wrap.dragging {{ cursor: grabbing; }}
    .mermaid-wrap .mermaid {{
      width: 100%; height: 100%; display: flex; align-items: center; justify-content: center;
    }}
    .mermaid-wrap .mermaid svg {{
      shape-rendering: geometricPrecision; text-rendering: optimizeLegibility;
    }}
    .zoom-controls {{
      position: absolute; top: 8px; right: 8px; display: flex; gap: 4px; z-index: 10;
    }}
    .zoom-btn {{
      width: 32px; height: 32px; border-radius: 6px; border: 1px solid var(--border);
      background: var(--bg2); color: var(--fg2); font-size: 0.95rem; cursor: pointer;
      display: flex; align-items: center; justify-content: center; transition: all 0.1s;
      font-family: -apple-system, 'Segoe UI', sans-serif; line-height: 1;
    }}
    .zoom-btn:hover {{ background: var(--bg3); color: var(--fg); border-color: var(--fg2); }}
    .zoom-label {{
      height: 32px; padding: 0 10px; border-radius: 6px; border: 1px solid var(--border);
      background: var(--bg); color: var(--fg2); font-size: 0.75rem;
      display: flex; align-items: center; font-family: 'SFMono-Regular', Consolas, monospace;
    }}
  </style>
</head>
<body>

<header>
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none">
    <rect width="24" height="24" rx="5" fill="#58a6ff" opacity="0.15"/>
    <rect x="4" y="4" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="13" y="4" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="4" y="13" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="13" y="13" width="7" height="7" rx="1.5" fill="#3fb950"/>
  </svg>
  <h1><span>{project_name}</span> Auto-Generated Diagrams</h1>
  <div class="header-badges">
    <span class="badge badge-auto">auto-generated {timestamp}</span>
  </div>
</header>

<div class="tabs" id="tabs">
{tabs_html}</div>

{panels_html}

<script>
  mermaid.initialize({{
    startOnLoad: false, theme: 'dark',
    themeVariables: {{
      background: '#0d1117', primaryColor: '#1f6feb', primaryTextColor: '#e6edf3',
      primaryBorderColor: '#30363d', lineColor: '#58a6ff', secondaryColor: '#161b22',
      tertiaryColor: '#0d1117', nodeBorder: '#30363d', clusterBkg: '#161b22',
      clusterBorder: '#30363d', titleColor: '#e6edf3', edgeLabelBackground: '#0d1117',
      fontFamily: "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif",
      fontSize: '16px', labelTextColor: '#e6edf3', nodeTextColor: '#e6edf3',
    }},
    flowchart: {{ curve: 'basis', padding: 24, nodeSpacing: 50, rankSpacing: 60, htmlLabels: true, wrappingWidth: 200 }},
  }});

  (async () => {{
    await mermaid.run({{ querySelector: '.mermaid' }});

    document.querySelectorAll('.mermaid-wrap').forEach(wrap => {{
      const svg = wrap.querySelector('.mermaid svg');
      if (!svg) return;
      const bbox = svg.getBBox();
      const pad = 30;
      const fullVB = {{ x: bbox.x - pad, y: bbox.y - pad, w: bbox.width + pad * 2, h: bbox.height + pad * 2 }};
      svg.setAttribute('viewBox', fullVB.x+' '+fullVB.y+' '+fullVB.w+' '+fullVB.h);
      svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
      svg.removeAttribute('width'); svg.removeAttribute('height');
      svg.style.width = '100%'; svg.style.height = '100%';

      const controls = document.createElement('div');
      controls.className = 'zoom-controls';
      controls.innerHTML = '<button class="zoom-btn" data-action="in">+</button><span class="zoom-label">Fit</span><button class="zoom-btn" data-action="out">&minus;</button><button class="zoom-btn" data-action="fit">&#8596;</button>';
      wrap.appendChild(controls);

      let vb = {{ ...fullVB }};
      const label = controls.querySelector('.zoom-label');

      function applyVB() {{
        svg.setAttribute('viewBox', vb.x+' '+vb.y+' '+vb.w+' '+vb.h);
        const fit = Math.abs(vb.w - fullVB.w) < 1 && Math.abs(vb.h - fullVB.h) < 1;
        label.textContent = fit ? 'Fit' : Math.round(fullVB.w / vb.w * 100) + '%';
      }}

      function zoomAt(f, cx, cy) {{
        const nw = vb.w/f, nh = vb.h/f, s = fullVB.w/nw;
        if (s < 0.15 || s > 10) return;
        vb.x = cx - (cx-vb.x)/vb.w*nw; vb.y = cy - (cy-vb.y)/vb.h*nh;
        vb.w = nw; vb.h = nh; applyVB();
      }}

      wrap.addEventListener('wheel', e => {{
        e.preventDefault();
        const r = svg.getBoundingClientRect();
        const rx = (e.clientX-r.left)/r.width, ry = (e.clientY-r.top)/r.height;
        zoomAt(e.deltaY<0?1.18:1/1.18, vb.x+rx*vb.w, vb.y+ry*vb.h);
      }}, {{passive:false}});

      let dragging=false, ds=null, vs=null;
      wrap.addEventListener('mousedown', e => {{
        if (e.target.closest('.zoom-controls')) return;
        dragging=true; wrap.classList.add('dragging');
        ds={{x:e.clientX,y:e.clientY}}; vs={{...vb}}; e.preventDefault();
      }});
      window.addEventListener('mousemove', e => {{
        if (!dragging) return;
        const r=svg.getBoundingClientRect();
        vb.x=vs.x-(e.clientX-ds.x)/r.width*vs.w;
        vb.y=vs.y-(e.clientY-ds.y)/r.height*vs.h; applyVB();
      }});
      window.addEventListener('mouseup', ()=>{{ dragging=false; wrap.classList.remove('dragging'); }});

      controls.addEventListener('click', e => {{
        const b=e.target.closest('.zoom-btn'); if(!b) return;
        const cx=vb.x+vb.w/2, cy=vb.y+vb.h/2;
        if(b.dataset.action==='in') zoomAt(1.4,cx,cy);
        if(b.dataset.action==='out') zoomAt(1/1.4,cx,cy);
        if(b.dataset.action==='fit') {{ vb={{...fullVB}}; applyVB(); }}
      }});
      applyVB();
    }});

    const panels = document.querySelectorAll('.panel');
    panels.forEach((p,i)=>{{ if(i!==0) p.classList.add('hidden'); }});
  }})();

  document.getElementById('tabs').addEventListener('click', e => {{
    const tab = e.target.closest('.tab');
    if (!tab) return;
    const idx = +tab.dataset.idx;
    document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i===idx));
    document.querySelectorAll('.panel').forEach((p,i)=>p.classList.toggle('hidden',i!==idx));
  }});
</script>
</body>
</html>'''


# ────────────────────────── SVG Rendering ──────────────────────────

def try_render_svg(dot_files: List[Tuple[str, str]], verbose: bool = False) -> int:
    dot_cmd = shutil.which("dot")
    if not dot_cmd:
        if verbose:
            print("Graphviz 'dot' not found on PATH - skipping SVG rendering")
        return 0
    count = 0
    for dot_path, svg_path in dot_files:
        try:
            subprocess.run([dot_cmd, "-Tsvg", "-o", svg_path, dot_path],
                           check=True, capture_output=True, timeout=30)
            count += 1
            if verbose:
                print(f"  Rendered: {svg_path}")
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            if verbose:
                print(f"  Failed: {e}")
    return count


# ────────────────────────── Main ──────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Generate crosspad-core architecture diagrams")
    parser.add_argument("--project-root", default=".", help="Project root directory")
    parser.add_argument("--output-dir", default=None, help="Output directory (default: docs/generated/)")
    parser.add_argument("--no-html", action="store_true", help="Skip HTML output")
    parser.add_argument("--no-dot", action="store_true", help="Skip DOT/SVG output")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    root = os.path.abspath(args.project_root)
    output_dir = args.output_dir or os.path.join(root, "docs", "generated")
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M")

    if args.verbose:
        print(f"Project root: {root}")
        print(f"Output dir:   {output_dir}\n")

    # 1. Discover modules
    modules = discover_modules(root, args.verbose)
    if not modules:
        print("ERROR: No modules found.", file=sys.stderr)
        sys.exit(1)

    # 2. Build dependency graph
    edges = build_module_graph(modules, args.verbose)

    # 3. Generate diagrams
    if args.verbose:
        print("\nGenerating diagrams...")

    mod_mermaid = generate_module_deps_mermaid(modules, edges)
    iface_mermaid = generate_interface_map_mermaid(modules)
    arch_mermaid = generate_architecture_layers_mermaid(modules)

    # 4. Write outputs
    os.makedirs(output_dir, exist_ok=True)

    md_files = [
        ("module_dependencies.md", "Module Dependencies", mod_mermaid),
        ("interface_map.md", "Interface Map", iface_mermaid),
        ("architecture_layers.md", "Architecture Layers", arch_mermaid),
    ]
    for filename, title, code in md_files:
        path = os.path.join(output_dir, filename)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f"# {title}\n\nAuto-generated on {timestamp}\n\n```mermaid\n{code}\n```\n")
        if args.verbose:
            print(f"  Wrote: {path}")

    dot_render = []
    if not args.no_dot:
        dot_outputs = [
            ("module_dependencies.dot", generate_module_deps_dot(modules, edges)),
            ("interface_map.dot", generate_interface_map_dot(modules)),
            ("architecture_layers.dot", generate_architecture_layers_dot(modules)),
        ]
        for filename, code in dot_outputs:
            path = os.path.join(output_dir, filename)
            with open(path, 'w', encoding='utf-8') as f:
                f.write(code)
            dot_render.append((path, path.replace('.dot', '.svg')))
            if args.verbose:
                print(f"  Wrote: {path}")

    if not args.no_html:
        html = generate_html({
            "module_dependencies": mod_mermaid,
            "interface_map": iface_mermaid,
            "architecture_layers": arch_mermaid,
        }, timestamp, "crosspad-core")
        html_path = os.path.join(output_dir, "diagrams.html")
        with open(html_path, 'w', encoding='utf-8') as f:
            f.write(html)
        if args.verbose:
            print(f"  Wrote: {html_path}")

    svg_count = try_render_svg(dot_render, args.verbose) if dot_render else 0

    file_count = len(md_files) + len(dot_render) + (1 if not args.no_html else 0) + svg_count
    print(f"\nGenerated {file_count} files in {output_dir}")
    print(f"  Modules: {len(modules)}")
    print(f"  Dependency edges: {len(edges)}")


if __name__ == "__main__":
    main()
