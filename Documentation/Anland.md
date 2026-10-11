<!--
title: Anland
section: Guides
order: 5
desc: A Wayland desktop on Android through Anland: install a patched compositor in the container, install the Anland app, and turn on Anland Display.
keywords: anland, wayland, droidspaces, android, kwin, weston, kde, plasma, gpu, adreno, snapdragon
-->

# Anland

Anland is a second way to get a desktop on the phone, next to Termux:X11. It is for Wayland. A Linux compositor patched for Anland, KWin for KDE Plasma or Weston, renders straight into GPU buffers, and the Anland app on Android shows those buffers on screen. Droidspaces runs the small daemon that connects the two over a socket and gives you a button to open the desktop.

Anland only handles the display. The container, its rootfs, GPU access and startup are still Droidspaces' job. Anland works best on Qualcomm Snapdragon devices with Adreno GPUs, because it leans on the device's GPU buffer sharing, and that is where it has been tested most.

## What you need

- A container that already runs on your phone.
- A compositor patched for Anland inside that container. KWin is the main one; the [Anland producers directory](https://github.com/superturtlee/anland/tree/main/producers) lists the backends that exist.
- The Anland app on Android.

## Step 1: Put an Anland backend in the container

Either build the backend yourself or use a rootfs that already has it.

### Build it inside the container

For a distribution or desktop of your choice, or when you want to see what the backend does.

1. Open a shell in the container and clone the Anland source:

   ```bash
   git clone https://github.com/superturtlee/anland.git
   ```

2. Under `producers`, pick the directory for your desktop and distribution. For KDE it looks like this:

   ```
   producers/kde
   ├── anland_backend_debian13_v5
   ├── anland_backend_v5
   ├── Debian13_v5
   ├── Fedora43_v5
   └── ubuntu2604_v5
   ```

3. Run that directory's `build.sh` as root. It installs the dependencies and the patched backend.
4. Start the desktop with the project's `startup.sh`.

### Use a prebuilt KDE Plasma rootfs

[Droidspaces-rootfs-KDE-builder](https://github.com/Goldzxcbug/Droidspaces-rootfs-KDE-builder) builds a KDE Plasma rootfs with the Anland backend already in it. It is the simpler route if KDE is what you want; it builds nothing else.

1. Fork the project and open its **Actions** page.
2. Run the "编译并发布Droidspaces Rootfs" workflow by hand with the "Wayland 支持" option turned on. The build takes about fifteen minutes.
3. Download the rootfs from the project's **Releases** page and install it with **Install from file** in the app.
4. If the desktop does not start on its own, enter the container as a normal user and run `startplasma-wayland`.

## Step 2: Install the Anland app

Install the APK from the [latest Anland release](https://github.com/superturtlee/anland/releases/latest), open it once, and grant it root.

## Step 3: Configure the container

In the container's configuration:

1. Turn on **GPU Access**. The Anland switch stays greyed out until it is on, or Hardware Access is.
2. Turn on **Anland Display**. On the CLI that is `--anland`, key `enable_anland`. Turning it on turns **Configure Termux:X11** off: the two display paths set conflicting variables and services, so a container uses one or the other.

Three more settings are worth knowing:

- **Configure PulseAudio** can stay off. Anland carries audio itself. If it was on before, remove any `PULSE_SERVER` variable left in the container so programs stop looking for the old server.
- **Allow Sandboxing** should be on for a full desktop such as KDE Plasma. Without it Plasma on Wayland can get slow or stop responding. Privileged mode is not needed, and neither is any of its tags.
- **SELinux Permissive** should stay off. Try enforcing first; turn permissive on only if Anland fails to start or draw on your device, and treat that as a workaround while you report it.

Start the container. As soon as the broker's socket is recorded, which happens as the container starts, its card on the **Panel** tab and its details page show a **Launch Anland** button, which opens the desktop in the Anland app. The desktop itself appears once the compositor inside the container has started.

---

Previous: [Display, audio and desktop](Graphics-and-Audio.md) | Next: [Kernel configuration](Kernel-Configuration.md)
