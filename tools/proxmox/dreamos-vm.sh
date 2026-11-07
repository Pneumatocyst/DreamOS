#!/usr/bin/env bash
#
# dreamos-vm.sh: make a DreamOS virtual machine on a Proxmox VE host.
#
# Run it on the Proxmox host, as root (the host's own shell, or the web UI's
# node > Shell):
#
#   ./dreamos-vm.sh               create the VM (stopped, ready to boot)
#   ./dreamos-vm.sh update        fetch the newest dream.iso for an existing VM
#
# Options (as environment variables, all optional):
#
#   VMID=150          the VM's ID          (default: the next free one)
#   NAME=dreamos      the VM's name        (default: dreamos)
#   ISO_STORAGE=local where the ISO goes   (default: the first storage that holds ISOs)
#   DISK_STORAGE=local-lvm                 (default: local-lvm, or the first storage for VM disks)
#   ISO_URL=...       a different build    (default: the latest build on GitHub)
#
# What it does: downloads dream.iso, checks its SHA-256, stores it as
# <ISO_STORAGE>:iso/dreamos.iso, and creates a VM with the hardware dream has
# drivers for:
#
#   machine  i440fx ("pc"), SeaBIOS   dream's disk driver speaks the old IDE controller
#   disk     1 GiB on IDE (ide0)       dream formats it on first boot and saves files to it
#   cd-rom   dreamos.iso on IDE (ide2) boots first, every time, so updates are just a new ISO
#   display  standard VGA              Limine sets up the framebuffer from it
#   mouse    PS/2 + vmmouse            the USB tablet is switched off (dream has no USB),
#                                      and vmmouse makes the pointer work in the web console
#   network  none                      dream has no network driver yet
#   serial   a socket                  the kernel log: `qm terminal <vmid>` to watch it
#
# Nothing else on the host is changed. Removing it later is just `qm destroy <vmid>`.

set -Eeuo pipefail

REPO="Pneumatocyst/DreamOS"
ISO_URL="${ISO_URL:-https://github.com/$REPO/releases/download/latest/dream.iso}"
ISO_NAME="dreamos.iso"
NAME="${NAME:-dreamos}"

say()  { printf '\033[1;35m::\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || fail "run this as root on the Proxmox host"
command -v qm >/dev/null && command -v pvesm >/dev/null || fail "this doesn't look like a Proxmox VE host (no qm / pvesm)"
command -v curl >/dev/null || fail "curl is needed (apt install curl)"

# ---- where things go ----------------------------------------------------------------------

first_storage_for() {          # first active storage that can hold this content type
    pvesm status --content "$1" 2>/dev/null | awk 'NR > 1 && $3 == "active" { print $1; exit }'
}

ISO_STORAGE="${ISO_STORAGE:-$(first_storage_for iso)}"
[[ -n "$ISO_STORAGE" ]] || fail "no storage here can hold ISO images. set ISO_STORAGE=..."
if [[ -z "${DISK_STORAGE:-}" ]]; then
    if pvesm status --content images 2>/dev/null | awk '{ print $1 }' | grep -qx local-lvm; then
        DISK_STORAGE=local-lvm
    else
        DISK_STORAGE="$(first_storage_for images)"
    fi
fi
[[ -n "$DISK_STORAGE" ]] || fail "no storage here can hold VM disks. set DISK_STORAGE=..."

# ---- the ISO -------------------------------------------------------------------------------

TMP=""                                 # global, so the clean-up below can still see it at exit
cleanup() { if [[ -n "$TMP" ]]; then rm -f "$TMP" "$TMP.sha256"; fi; }
trap cleanup EXIT

fetch_iso() {
    local dest
    dest="$(pvesm path "$ISO_STORAGE:iso/$ISO_NAME")"
    mkdir -p "$(dirname "$dest")"
    TMP="$(mktemp "$(dirname "$dest")/.dreamos.XXXXXX")"
    local tmp="$TMP"

    say "downloading $ISO_URL"
    curl -fL --retry 3 --progress-bar -o "$tmp" "$ISO_URL" || fail "couldn't download the ISO"

    # the release publishes a checksum next to the ISO; use it when it's there
    if curl -fsSL --retry 3 -o "$tmp.sha256" "$ISO_URL.sha256" 2>/dev/null; then
        local want got
        want="$(awk '{ print $1 }' "$tmp.sha256")"
        got="$(sha256sum "$tmp" | awk '{ print $1 }')"
        [[ "$want" == "$got" ]] || fail "the ISO's checksum doesn't match (expected $want, got $got)"
        say "checksum ok ($got)"
    else
        say "no checksum published next to the ISO; skipping that check"
    fi
    # every ISO 9660 image says "CD001" at byte 32769: a cheap check we didn't get an error page
    [[ "$(dd if="$tmp" bs=1 skip=32769 count=5 2>/dev/null)" == "CD001" ]] || fail "that download isn't an ISO image"

    chmod 644 "$tmp"
    mv -f "$tmp" "$dest"               # replaced in one step: a running VM never sees half a file
    rm -f "$tmp.sha256"
    TMP=""
    say "stored as $ISO_STORAGE:iso/$ISO_NAME"
}

find_vm() {                            # the VMID of an existing VM with our name, if any
    { qm list 2>/dev/null || true; } | awk -v n="$NAME" 'NR > 1 && $2 == n { print $1; exit }'
}

# ---- update: just a fresh ISO -----------------------------------------------------------------

if [[ "${1:-create}" == "update" ]]; then
    existing="$(find_vm)"
    [[ -n "$existing" ]] || fail "there's no VM called $NAME yet. run this without 'update' first"
    fetch_iso
    if qm status "$existing" | grep -q running; then
        say "VM $existing is running: restart it to boot the new build (your files on its disk stay)"
    else
        say "done. start VM $existing to boot the new build"
    fi
    exit 0
fi
[[ "${1:-create}" == "create" ]] || fail "unknown command '$1' (try: create, update)"

# ---- create ---------------------------------------------------------------------------------------

existing="$(find_vm)"
[[ -z "$existing" ]] || fail "VM $existing is already called $NAME. use '$0 update' for a new build, or NAME=... for another VM"

VMID="${VMID:-$(pvesh get /cluster/nextid)}"
qm status "$VMID" >/dev/null 2>&1 && fail "VMID $VMID is taken. pick another with VMID=..."

fetch_iso

say "creating VM $VMID ($NAME): disk on $DISK_STORAGE, ISO from $ISO_STORAGE"
qm create "$VMID" \
    --name "$NAME" \
    --description "DreamOS, a hobby x86_64 OS (https://github.com/$REPO). Update the ISO with tools/proxmox/dreamos-vm.sh update." \
    --ostype other \
    --machine pc \
    --bios seabios \
    --cores 1 --sockets 1 \
    --memory 512 --balloon 0 \
    --vga std \
    --tablet 0 \
    --ide0 "$DISK_STORAGE:1" \
    --ide2 "$ISO_STORAGE:iso/$ISO_NAME,media=cdrom" \
    --boot "order=ide2;ide0" \
    --serial0 socket \
    --onboot 0

# vmport is QEMU's default on this machine type; say so explicitly so the pointer
# integration (vmmouse) can't silently disappear with a future default change.
# Setting raw QEMU args needs root@pam, which is who runs this script.
qm set "$VMID" --args "-machine vmport=on" >/dev/null

say "done. VM $VMID ($NAME) is ready, and stopped."
echo
echo "  start it:       qm start $VMID      (or the Start button in the web UI)"
echo "  use it:         the web UI's Console tab. the desktop starts by itself."
echo "  kernel log:     qm terminal $VMID   (ctrl+o to leave)"
echo "  newer build:    $0 update"
echo "  remove it:      qm stop $VMID && qm destroy $VMID"
