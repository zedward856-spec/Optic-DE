# Setting up Optic DE on a Debian PC

This guide installs Optic DE on **Debian 12 (bookworm)** or **Debian 13 (trixie)**, on a normal
PC or laptop, and starts it either from your login screen or with `startx`. Ubuntu and other
Debian-based systems work the same way.

Optic DE is an **X11** desktop. It doesn't run on Wayland, but you can keep a Wayland desktop
(GNOME, KDE) installed alongside it and pick Optic at the login screen.

---

## 1. Install what it needs

```sh
sudo apt update
sudo apt install \
  build-essential pkg-config git \
  libx11-dev libxft-dev libxext-dev libxcursor-dev libpng-dev libfontconfig-dev \
  xorg xinit dbus-x11 x11-xserver-utils x11-utils \
  xterm fonts-dejavu-core adwaita-icon-theme \
  firefox-esr nano xclip
```

| Packages | Why |
|---|---|
| `build-essential pkg-config` and the `lib…-dev` packages | Compile Optic DE |
| `xorg xinit` | The X server, and `startx` |
| `dbus-x11` | A session bus for the apps (`dbus-launch`) |
| `x11-xserver-utils x11-utils` | `xrdb`, and `xrandr` (Settings → Screen size) |
| `xterm` | Optic Terminal's tabs are xterms |
| `fonts-dejavu-core adwaita-icon-theme` | Terminal font, and the pointer |
| `firefox-esr` | The browser on the taskbar |
| `nano xclip` | Optic Files: editing text files, and "Copy path" |

`firefox-esr`, `nano` and `xclip` are optional. Optic DE runs without them; those buttons just won't do anything.

## 2. Get it and build it

```sh
git clone https://github.com/zedward856-spec/Optic-DE.git
cd Optic-DE
make
sudo make install
```

`make install` puts everything in **`/opt/optic-de`**, links the commands into
`/usr/local/bin`, installs the BPreplay font, and adds an **Optic** entry to
`/usr/share/xsessions` for login screens.

## 3. Start it

### Option A: from your login screen (LightDM, GDM, SDDM…)
1. Log out.
2. On the login screen, open the session menu (usually a gear or a "Session" list) and choose **Optic**.
3. Log in.

### Option B: with `startx` (no login manager, or from a text console)

```sh
echo 'exec optic-session' > ~/.xinitrc
startx
```

Leave Optic DE with **Ctrl+Alt+Backspace** if that's enabled, or close the session from a
terminal with `pkill -x optic-de`.

## 4. Make it the right size (HiDPI screens)

Optic DE draws at normal size on a PC. On a high-resolution screen (a 4K monitor, a 13" laptop
at 2560 px or wider), make everything twice as big:

- **Login-screen start (option A):** add this line to `~/.xsessionrc`:
  ```sh
  export OPTIC_SCALE=2
  ```
- **startx (option B):** put it before the `exec` line in `~/.xinitrc`:
  ```sh
  export OPTIC_SCALE=2
  exec optic-session
  ```

`OPTIC_SCALE` scales Optic DE, GTK apps (Firefox), the terminal font and the pointer together.

## 5. The Optic look for GTK apps (optional)

Optic DE comes with a black GTK theme for Firefox and other GTK apps. If you already have GTK
settings (`~/.config/gtk-3.0/gtk.css` or `settings.ini`), Optic **leaves them alone**. To use Optic's
anyway, set this the same way as `OPTIC_SCALE` above:

```sh
export OPTIC_THEME=always
```

## 6. Using it

- **Taskbar:** move the pointer to the bottom edge and the pill slides in. It hides again after a few seconds. Settings → Taskbar → *Always shown* keeps it up.
- **Optic button** (far left of the taskbar): every installed app.
- **Windows:**
  - drag the title bar to move, drag an edge or bottom corner to resize;
  - double-click the title to maximise.
- **Snapping:**
  - drag a window to the **top** of the screen and drop it on a layout in the card that appears;
  - drag it to the **left or right edge** for half the screen, or near a corner for a quarter.
- **Terminal:** **+** or Ctrl+Shift+T for a new tab.
- **Files:**
  - double-click to open, right-click for the menu;
  - Ctrl+H shows hidden files, Ctrl+L types a path.
- **Settings:**
  - wallpaper (Optic stone or black);
  - taskbar (hides itself, or always shown);
  - clock seconds;
  - **Preload Firefox**: starts Firefox in the background at login so it opens instantly, at the cost of about 250 MB of RAM. Switch it off on low-memory machines.

## 7. Updating and removing

```sh
cd Optic-DE
git pull
make
sudo make install     # then log out and back in
```

To remove it:

```sh
sudo make uninstall
```

This removes `/opt/optic-de`, the font, the command links and the login-screen entry. Your
`~/.config/optic-de.conf` settings file is kept; delete it by hand if you want.

## Troubleshooting

| Problem | What to try |
|---|---|
| Login screen has no **Optic** entry | Check that `/usr/share/xsessions/optic.desktop` exists (rerun `sudo make install`). Some login screens only list sessions after a restart. |
| Black screen, then back to the login screen | Look at `~/.xsession-errors` (login screen) or the terminal you ran `startx` from. A missing `dbus-x11` is the usual cause. |
| Everything is tiny | Set `OPTIC_SCALE=2` (step 4). |
| Terminal opens nothing | Install `xterm`; Optic Terminal's tabs are xterms. |
| `make` fails with "Package … was not found" | One of the `lib…-dev` packages from step 1 is missing. |
| Wayland login (GNOME / KDE) shows no Optic | Choose Optic from the login screen's session list; it starts its own X11 session. |

Optic DE supports a single monitor for now. With two screens, it treats them as one large desktop.
