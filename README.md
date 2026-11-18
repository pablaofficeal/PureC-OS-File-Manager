# PureC-OS-File-Manager

Graphical file manager of PureC OS (`files`, `/usr/bin/files`), written in C on
libgui (PureGUI, `pg/*.h`) and the video-backend display server.

## Features

- sidebar: Home and its Desktop/Documents/Downloads, `/`, `/usr`, `/etc`,
  `/tmp`, `/var` and the volumes listed in `/proc/mounts` (only existing folders)
- toolbar: Back, Forward, Up, breadcrumb path bar (click a segment to jump,
  click the empty part or press Ctrl+L to type a path), filter field, List/Grid
- list view with sortable columns (name, size, modified, type) and grid view
  with large icons; both are virtualised, directories load incrementally so the
  UI never blocks on big folders
- selection by click, Ctrl+click, Shift+click, Ctrl+A and a rubber band
- new folder, new file, rename, delete with confirmation, cut/copy/paste with
  recursive copy, conflict dialog (skip, replace, keep both, apply to all) and
  a progress dialog with Cancel; operations run in a worker thread
- properties dialog: size (recursive for folders), type, link target, times,
  owner and group, permission checkboxes with Apply
- context menus for items and for empty space
- icons are drawn with canvas primitives (folder, file, text, code, image,
  audio, archive, executable, package, drive, symlink badge), no assets
- status bar: item count, selection size, free space (`statvfs`), hidden files toggle

## Keys

| Key | Action |
| --- | --- |
| Enter, double click | open |
| Backspace, Alt+Up | parent folder |
| Alt+Left / Alt+Right | history back / forward |
| Arrows, Home, End, PgUp, PgDn | move (Shift extends, Ctrl moves only) |
| letters | type-to-select |
| Space | toggle selection of the cursor item |
| Ctrl+A | select all |
| Ctrl+L | edit the path |
| Ctrl+F | focus the filter (Esc clears it) |
| Ctrl+H | toggle hidden files |
| Ctrl+G | switch list/grid |
| F5 | refresh |
| F2 | rename |
| Delete | delete |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Ctrl+Shift+N | new folder |
| Alt+Enter | properties |
| Ctrl+Q | quit |

## Opening files

Rules come from `~/.config/files/open.conf`, then `/etc/files/open.conf`, then
built-in defaults. First match wins.

    # extensions = action
    txt,md,c,h = term:vim %f
    pkg.tar.gz = pkg
    executable = run
    default = term:vim %f

Extensions are comma separated and matched against the end of the file name
(multi-part ones such as `pkg.tar.gz` work). Actions:

- `term:COMMAND` run in a new terminal window, which closes when the command ends
- `termwait:COMMAND` the same, but waits for Enter before closing
- `exec:COMMAND` start directly without a terminal
- `run` run the file itself in a terminal
- `pkg` ask for confirmation, then run `pkg -U FILE`

`%f` is replaced by the quoted path (it is appended when absent). `executable`
applies to files with an execute bit and no matching rule; `default` applies to
everything else, without it an "Open with" dialog asks for a command.

The terminal has no command-line interface, so `files` starts `terminal` with
`SHELL=/usr/bin/files` and `FILES_RUN` in the environment; in that mode the
binary runs the command and exits instead of opening a window.

## Build

    make ROOT_DIR=/path/to/PureC-OS
    make ROOT_DIR=/path/to/PureC-OS package

The package contains `usr/bin/files`, `usr/share/applications/files.desktop`
and `etc/files/open.conf`.
