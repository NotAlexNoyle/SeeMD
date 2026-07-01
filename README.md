# ViewMD

A lightweight GTK 3 markdown viewer and editor for desktop Linux. It is ideal as your default app for opening `.md` files. Uses the excellent library [md4c](https://github.com/mity/md4c) for markdown parsing and WebKitGTK for GitHub-style preview rendering. This project is a fork of [ViewMD](https://github.com/rabfulton/ViewMD) by NotAlexNoyle.

![ViewMD](assets/screenshot.png)

## Features

- **GitHub-style preview** - Rendered Markdown and inline HTML are displayed through WebKitGTK with GitHub-like markdown styling
- **Read-only preview** - Rendered Markdown stays non-editable until source mode is enabled
- **Source editing mode** - Toggle between rendered preview and editable Markdown
- **Minimal UI** - Clean toolbar with open, reload, edit, save, and settings buttons
- **Lightweight** - Pure C with GTK and WebKitGTK, fast startup
- **Hyperlink support** - Left click opens links and internal anchors
- **Document search** - `Ctrl+F` with next/previous match navigation
- **Local image support** - Local images keep natural size unless they need to fit the document window

## Supported Markdown

| Syntax | Description |
|--------|-------------|
| `# Header 1` | Large bold header |
| `## Header 2` | Medium bold header |
| `### Header 3` | Small bold header |
| `**bold**` | Bold text |
| `*italic*` | Italic text |
| `` `code` `` | Inline code |
| <code>```...```</code> | Code block |
| `- item` | List item |
| `> quote` | Block quote |
| `[text](url)` | Link |
| `~~strike~~` | Strikethrough |
| `\| table \| row \|` | Markdown tables |
| `<strong>HTML</strong>` | GitHub-style inline and block HTML rendered in preview mode |
| `---` | Horizontal rule |

Code blocks currently support a beta version of keyword highlighting for fenced languages `c`, `java`, and `python`.

## Installation

### Arch Linux (AUR)

https://aur.archlinux.org/packages/viewmd

### Debian/Ubuntu (.deb)

Install a built `.deb` package with:

```bash
sudo apt install ./viewmd_*.deb
```

### Fedora/RHEL (.rpm)

Install a built `.rpm` package with:

```bash
sudo dnf install ./viewmd-*.rpm
```

## Usage

Run `viewmd` to start the application.

- **Open button**: Open a markdown document
- **Reload button**: Reload the currently open document from disk
- **Edit button**: Toggle editable Markdown source mode (`Ctrl+E`)
- **Save button**: Save the current Markdown source (`Ctrl+S`)
- **Settings button**: Adjust theme, fonts, and markdown accent colors

### Find in Document

- Press `Ctrl+F` to open search.
- Type to highlight matches as you search.
- Press `Enter` for next match and `Shift+Enter` for previous match.
- Press `Esc` to close search.

### Set as Default `.md` Viewer

After installing, associate markdown MIME types with `viewmd.desktop`:

```bash
xdg-mime default viewmd.desktop text/markdown
xdg-mime default viewmd.desktop text/x-markdown
```

Verify the current default:

```bash
xdg-mime query default text/markdown
```

Test by opening a markdown file through your desktop association:

```bash
xdg-open README.md
```


## Building From Source

```bash
make
sudo make install
```

This installs:
- Binary to `/usr/local/bin/viewmd`
- Desktop file to `/usr/local/share/applications/viewmd.desktop`

### Uninstallation

```bash
sudo make uninstall
```
### Dependencies

### Arch Linux
```bash
sudo pacman -S gtk3 webkit2gtk-4.1
```

### Ubuntu/Debian
```bash
sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev
```

### Fedora
```bash
sudo dnf install gtk3-devel webkit2gtk4.1-devel
```

## Packaging Templates

- Arch: `packaging/arch/PKGBUILD`
- Debian: `packaging/deb/control.in`
- RPM: `packaging/rpm/viewmd.spec.in`
## License

Released under the [MIT License](LICENSE).
