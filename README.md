<div align="center">

<img src="logo/jamd-icon-light.png" alt="JAMD Editor" width="96" height="96">

# Just Another MarkDown Editor — **JAMD Editor**

A fast, 0.8 MB single‑executable **Markdown viewer & editor** for Windows.
Viewer‑first, fully offline, no install — one native `.exe` powered by Win32 + WebView2.
Editor as add-on for Total Commander viewer plugin WLX at [JAMD-WLX](https://github.com/mikinko/JustAnotherMarkdown_lister_WLX)

</div>

---

## News — v1.6: Pele diagrams

![Pele-rendered Gantt and Git graph diagrams in JAMD](scrshot/pele-mermaid.png)

Mermaid diagrams can now be drawn by **[Pele](https://pele.run)**, a new
renderer that is about **5× faster** than Mermaid and one-eighth of its size.
Its colours follow the JAMD theme, so light/dark switches recolour diagrams
instantly.

- **Settings ▸ Diagrams**: Mermaid or Pele (`Diagrams=mermaid|pele` in `JAMD.ini`).
  58 sample diagrams render in 275 ms with Pele instead of 1,600 ms with Mermaid.
- **Add diagram** button (Pele logo) in the toolbar: a starter for any of
  **32 diagram types**: flowchart, Gantt, sequence, Git graph, mind map,
  timeline, C4, Kanban, Sankey, …
- **Check for updates** under *Diagrams*: compares Mermaid and Pele with npm and
  updates them in place, without a restart.

**Also new in 1.6**
- **Version history**: a copy per save, line diff, one-click restore
- **Change bars**: what changed since the last save, in the source and the preview
- **`[[Wiki links]]`** with autocomplete, and **backlinks** under the contents
- **Front matter** shown as a properties card
- **Section path** above the document, **scrollbar marks** for headings, find hits and warnings
- **Document checks**: heading jumps, duplicate headings, dead anchors, missing alt text
- **Smart paste** of Excel cells as a table, **drop an image** to copy it into `img\`
- **Drag a contents entry** to move its whole section
- `Ctrl+Shift+P` **command palette**, `Ctrl+Shift+M` **focus mode**, `Ctrl+Shift+C` **copy as rich text**
- Optional features are one 0/1 key each in `JAMD.ini` (see `JAMD.ini.sample`)

**Faster** (6,000-block document, split view)

| | 1.5 | 1.6 |
|---|---|---|
| Re-render after a keystroke | ~650 ms | ~150 ms |
| 40 scroll jumps | ~6.4 s | ~1.4 s |
| First paint | ~1,150 ms | ~750 ms |

- Only the blocks you edited are re-rendered.
- The panes are layout boundaries, so moving the contents marker no longer
  re-lays the whole preview.
- Prism loads only when a document needs it.
- Changes on disk are detected immediately, without polling.

**Safer**
- Links can no longer start programs.
- Saves are atomic.
- ANSI and UTF-16 files open correctly.
- Untitled tabs come back after a crash.

**Libraries**: Mermaid 12.2, KaTeX 0.19, markdown-it 15.0.3, DOMPurify 3.4.17.

Earlier: tabs for multiple files (1.4) and YAML / TOML support.

---

![Split editor with formatting toolbar](scrshot/split-view.png)

## Why another Markdown editor?

JAMD is built to be the app which opens instantly,
renders GitHub‑Flavored Markdown exactly, and lets you drop into a live source editor with
**Ctrl+E** when you actually want to change something. No Electron, no telemetry, no cloud —
a single ~2 MB executable with its assets beside it. Working with AI models, MD files are everywhere, all editors are huge, cloud, whatever. So this is quick, small, do what it should do. 

## Features

**Rendering**
- Full GitHub‑Flavored Markdown: tables, task lists, footnotes, strikethrough
- GitHub‑style **alerts** (`> [!NOTE]`, `[!TIP]`, `[!WARNING]`, …)
- **Mermaid** diagrams (Mermaid or the faster **Pele** renderer) and **KaTeX** math — rendered locally, no network
- Syntax highlighting (Prism) with several selectable code themes
- Data views: JSON / YAML / TOML rendered as a collapsible tree; sandboxed HTML preview

**Editing**
- Toggleable split editor with a **line‑accurate scroll sync** and a caret‑position marker
- Formatting **toolbar**: undo/redo · bold · italic · strikethrough · heading · quote ·
  bullet / numbered / checklist · insert link, image, table, code block
- Smart typing: list/quote continuation on Enter, table cell navigation with Tab, auto‑indent
- **Find & replace** with match‑case, wrap, replace‑all
- Paste an image → stored next to the document under `img/`; or insert one from a file
- Preserves the document's original encoding (BOM) and EOL style (CRLF/LF) on save

**Workflow**
- **Quick open** (Ctrl+K) across recent files and the current folder
- Recent‑files list, drag & drop, open a file as a launch argument (TC F3 viewer)
- **Presentation mode** (F5) — slides split on `---` or headings
- Export to **HTML** and **PDF**
- Autosave crash‑recovery and external‑change detection (reload prompt)
- Themes: GitHub Light / Dark / system‑auto, plus JSON theme presets in `themes/`
  (Dracula, Nord, Tokyo Night, One Dark, Gruvbox, Solarized)
- Table of contents, adjustable zoom & reading width, line numbers

## Screenshots

| Reading view (dark) | Reading view (light) |
| --- | --- |
| ![dark](scrshot/preview-dark.png) | ![light](scrshot/preview-light.png) |

## Keyboard shortcuts

| Key | Action | | Key | Action |
| --- | --- | --- | --- | --- |
| `Ctrl+O` | Open | | `Ctrl+K` | Quick open |
| `Ctrl+S` | Save | | `Ctrl+H` | Find & replace |
| `Ctrl+Shift+S` | Save As | | `Ctrl+T` | Table of contents |
| `Ctrl+E` | Toggle source / split | | `Ctrl+L` | Line numbers |
| `Ctrl+D` | Toggle theme | | `Ctrl+G` | Scroll to top |
| `F5` | Presentation mode | | `Ctrl+Q` | Sync the two panes |
| `F1` | Help | | `Esc` | Close / back |

In the editor: `Ctrl+B` bold · `Ctrl+I` italic · `Ctrl+Shift+F` reformat table ·
`Tab` cell navigation / indent · `Enter` continues lists and quotes ·
`Ctrl+Shift+P` command palette · `Ctrl+Shift+M` focus mode · `Ctrl+Shift+C` copy as rich text.
`F1` in the app lists every shortcut.

## if you want buy me coffee , ill be glad, if not, enjoy anyway
![buyMeCoffee](scrshot/qr-code_resize_300x300.png)  
or here
[Buy Me a Coffee](https://buymeacoffee.com/mikinko)


## Building

Requirements:
- Windows 10/11
- Visual Studio 2022 (MSVC toolset) and **CMake ≥ 3.16**
- **WebView2 Runtime** (pre‑installed on Windows 11) and the WebView2 SDK NuGet
  package `Microsoft.Web.WebView2.1.0.2592.51` restored under `build/packages/`

```sh
cmake -B build -A x64
cmake --build build --config Release
```

The result is a self‑contained `build/Release/JAMDedit.exe` (static CRT) with `lib/` and
`themes/` copied beside it. The frontend (`src/index.html`) and app icon/logo are embedded
as resources; only the drop‑in `lib/` libraries and `themes/` presets live on disk.

## Usage

```sh
JAMDedit.exe path\to\file.md
```

No argument opens an empty buffer. To use it as the Total Commander **F3** viewer or the
default `.md` program, just point it at `JAMDedit.exe` — it takes a single path argument.

## Architecture

A single WebView2 fills the window; **all UI is HTML**, and the source editor is an in‑page
`<textarea>`, which gives native undo/redo and same‑DOM scroll sync for free. The native
Win32 side is deliberately thin: window + dark titlebar, WebView2 lifecycle, file dialogs and
UTF‑8 I/O, the settings INI, and a small string message protocol between page and host.
Documents and assets are served to the page over a synthetic origin via `WebResourceRequested`.

## License

Not yet licensed — add a `LICENSE` file to declare usage terms.
