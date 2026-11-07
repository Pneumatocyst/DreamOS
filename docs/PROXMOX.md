# Running dream on Proxmox VE

`tools/proxmox/dreamos-vm.sh` makes a DreamOS VM on a Proxmox host in one go: it
downloads the newest `dream.iso` from this repository's releases and creates a VM
with the hardware dream has drivers for. After that you just start it.

## Set it up (once)

On the Proxmox host, as root (an SSH session, or the web UI: your node > **Shell**):

```bash
curl -fsSLO https://raw.githubusercontent.com/Pneumatocyst/DreamOS/main/tools/proxmox/dreamos-vm.sh
```

```bash
bash dreamos-vm.sh
```

It prints the new VM's ID (the next free one, unless you set `VMID=...`). Then:

- **start it**: the **Start** button in the web UI, or `qm start <vmid>`
- **use it**: the VM's **Console** tab. dream boots straight into its desktop
- **watch the kernel log**: `qm terminal <vmid>` (ctrl+o leaves)

The mouse works in the web console without clicking into it first: dream speaks
QEMU's "vmmouse" protocol, so the pointer just follows yours.

## Newer builds

Every push to `main` builds a fresh ISO (GitHub Actions, `.github/workflows/iso.yml`),
checks that it boots, and publishes it as the `latest` release. To use it:

```bash
bash dreamos-vm.sh update
```

That replaces the ISO and nothing else; restart the VM to boot it. Files you made in
dream live on the VM's own disk, so they survive updates.

## What the VM looks like, and why

| setting | value | why |
|---|---|---|
| machine | `pc` (i440fx), SeaBIOS | dream's disk driver speaks the classic IDE controller |
| disk | 1 GiB on `ide0` | dream formats it on first boot and saves your files there |
| CD | `dreamos.iso` on `ide2`, first in the boot order | every boot runs the newest ISO |
| display | standard VGA | Limine takes the framebuffer from it |
| mouse | USB tablet **off**, `-machine vmport=on` | dream has no USB driver; vmmouse gives an absolute pointer |
| memory, CPU | 512 MiB, 1 core | dream uses one CPU and about 20 MiB |
| network | none | there's no network driver yet |
| serial | a socket | the kernel log goes there (`qm terminal`) |

Settings you can change: `VMID`, `NAME` (default `dreamos`), `ISO_STORAGE` (default: the
first storage that holds ISOs), `DISK_STORAGE` (default `local-lvm`), and `ISO_URL` (to
boot a build of your own). For example:

```bash
VMID=150 DISK_STORAGE=local-zfs bash dreamos-vm.sh
```

## If something's off

| | |
|---|---|
| the pointer doesn't move | under **Options**, *Use tablet for pointer* should be **No**, and `qm config <vmid>` should show `args: -machine vmport=on` |
| the super key opens your own computer's menu | use **f1** for dream's launcher instead |
| it doesn't boot at all | `qm terminal <vmid>` shows how far it got; if it stops early, try **Hardware > Processors > Type: qemu64**, then **KVM hardware virtualization: No** under Options (slow, but rules out a KVM quirk) |
| start over | `qm stop <vmid> && qm destroy <vmid>`, then run the script again |

Removing the VM doesn't touch anything else on the host; the ISO stays in your ISO
storage until you delete it.
