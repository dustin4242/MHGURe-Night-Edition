#!/usr/bin/env bash
# wifi-disable-audit.sh  (v2)
#
# READ-ONLY audit that hunts down every file-based way a Wi-Fi chip can be
# disabled on an Arch/CachyOS + systemd + NetworkManager system.
#
# Nothing is modified. Safe to run as a normal user (root gives fuller output).
#
#   bash wifi-disable-audit.sh
#   sudo bash wifi-disable-audit.sh > audit.txt 2>&1
#
# Sections 0-13  = the common mechanisms.
# Sections 14-16 = the ones people forget, plus "find it by content" and
#                  "diff the system against the past".

set -u

KW='iwlwifi|iwlmvm|iwldvm|ath[0-9]|ath1[012]k|rtw[0-9]|rtl[0-9]|mt7[0-9]|mt76|brcm|bcma|cfg80211|mac80211|wlan|wifi|wireless|rfkill|80211'
CMDKW='blacklist|module_blacklist|pci-stub|vfio|rd.driver|rd.dracut|initcall_blacklist'

hdr()  { printf '\n\033[1m===== %s =====\033[0m\n' "$*"; }
sub()  { printf '\n--- %s ---\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
note() { printf '  %s\n' "$*"; }

# Print a small config dir file-by-file (skip anything large/binary).
dumpdir() {
    local d=$1 f n=0
    [ -d "$d" ] || return 0
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        [ "$(stat -c%s "$f" 2>/dev/null || echo 999999)" -gt 20000 ] && continue
        printf '\n--- %s ---\n' "$f"
        sed 's/^/    /' "$f" 2>/dev/null
        n=$((n+1))
    done
    [ "$n" -eq 0 ] && note "(empty or no plain files)"
    return 0
}

# grep -rInE over a tree, indented. Prints nothing if no match.
greptree() {
    local d=$1 re=$2
    [ -d "$d" ] || return 0
    grep -rInE -- "$re" "$d" 2>/dev/null | sed 's/^/  /'
    return 0
}

# ----------------------------------------------------------------------------
hdr "0. WHAT AM I LOOKING AT"
sub "OS / kernel"
sed 's/^/  /' /etc/os-release 2>/dev/null | head -3
note "kernel: $(uname -r)"
sub "Wi-Fi hardware (look at 'Kernel driver in use' / 'Kernel modules')"
if have lspci; then
    lspci -nnk 2>/dev/null | grep -iA3 -E 'network|wireless' | sed 's/^/  /' || note "(no PCI wifi/ethernet device found)"
else
    note "lspci missing"
fi
if have lsusb; then
    lsusb 2>/dev/null | grep -iE '802\.11|wireless lan|wlan|wi-?fi' | sed 's/^/  /' || note "(no USB wifi dongle found)"
fi
sub "Network interfaces"
ls /sys/class/net/ 2>/dev/null | tr '\n' ' '; echo

# ----------------------------------------------------------------------------
hdr "1. CURRENT RUNTIME STATE (tells you WHICH layer is doing it)"
sub "rfkill"
if have rfkill; then rfkill list 2>/dev/null | sed 's/^/  /'; else note "rfkill missing"; fi
sub "NetworkManager radio"
if have nmcli; then nmcli radio 2>/dev/null | sed 's/^/  /'; nmcli device status 2>/dev/null | sed 's/^/  /'; fi
sub "Loaded wifi-related modules"
lsmod 2>/dev/null | grep -iE 'cfg80211|mac80211|iwlwifi|iwlmvm|ath|rtw|rtl8|mt76|mt7|brcm|bcma' | sed 's/^/  /' \
    || note "(none loaded -- consistent with a module blacklist, firmware block, or unplugged/authorized=0 device)"
sub "Kernel messages about wifi at this boot"
if have journalctl; then
    journalctl -k -b --no-pager 2>/dev/null | grep -iE 'rfkill|iwlwifi|cfg80211|firmware.*(fail|error)' | tail -15 | sed 's/^/  /' \
        || note "(nothing)"
fi

# ----------------------------------------------------------------------------
hdr "2. KERNEL MODULE LAYER  (most common way: blacklist / install false)"
sub "/etc/modprobe.d  -- FULL CONTENTS (your most likely culprit)"
dumpdir /etc/modprobe.d
sub "Every blacklist / install directive in effect (this is the disabling mechanism)"
if have modprobe; then
    out=$(modprobe --showconfig 2>/dev/null | grep -E '^(blacklist|install)')
    if [ -n "$out" ]; then printf '%s\n' "$out" | sed 's/^/  /'; else note "(none)"; fi
    sub "options / softdep lines mentioning wifi drivers"
    out=$(modprobe --showconfig 2>/dev/null | grep -E '^(options|softdep)' | grep -iE "$KW")
    if [ -n "$out" ]; then printf '%s\n' "$out" | sed 's/^/  /'; else note "(none)"; fi
fi
sub "Matches in all modprobe.d locations"
for d in /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d; do
    greptree "$d" "blacklist|install .*/bin/(false|true)|pci-stub|vfio"
done
sub "Does a wifi driver even EXIST for the running kernel?"
note "modules dir: /usr/lib/modules/$(uname -r)"
if [ -d "/usr/lib/modules/$(uname -r)/kernel/drivers/net/wireless" ]; then
    ls "/usr/lib/modules/$(uname -r)/kernel/drivers/net/wireless" 2>/dev/null | tr '\n' ' '; echo
else
    note "(no kernel/drivers/net/wireless in this kernel's module tree -- wrong/trimmed kernel package)"
fi
sub "/etc/modules-load.d  -- FULL CONTENTS (module *forced* to load)"
dumpdir /etc/modules-load.d
[ -f /etc/modules ] && { printf '\n--- /etc/modules ---\n'; sed 's/^/    /' /etc/modules; }

# ----------------------------------------------------------------------------
hdr "3. INITRAMFS LAYER  (module kept out of / baked blacklist into initramfs)"
sub "mkinitcpio main config"
if [ -f /etc/mkinitcpio.conf ]; then
    grep -nE '^(MODULES|HOOKS|FILES)=' /etc/mkinitcpio.conf | sed 's/^/  /'
    note "('modconf' in HOOKS = /etc/modprobe.d is copied into the initramfs)"
else
    note "/etc/mkinitcpio.conf not present"
fi
sub "/etc/mkinitcpio.conf.d"
dumpdir /etc/mkinitcpio.conf.d
sub "dracut drop-ins (only if you use dracut)"
if [ -d /etc/dracut.conf.d ]; then
    greptree /etc/dracut.conf.d 'omit_drivers|force_drivers|blacklist'
    dumpdir /etc/dracut.conf.d
else
    note "/etc/dracut.conf.d not present (mkinitcpio system)"
fi
sub "Debian-style initramfs (not applicable here, listed for completeness)"
[ -d /etc/initramfs-tools ] && greptree /etc/initramfs-tools "$KW" || note "not present"
sub "Is a blacklist actually baked into the initramfs image?"
found=0
for img in /boot/initramfs-linux*.img; do
    [ -f "$img" ] || continue
    found=1
    if have lsinitcpio; then
        note "$img:"
        lsinitcpio "$img" 2>/dev/null | grep -iE 'modprobe\.d' | sed 's/^/    /'
    fi
done
[ "$found" -eq 0 ] && note "(no /boot/initramfs-linux*.img found -- /boot may not be mounted here)"

# ----------------------------------------------------------------------------
hdr "4. KERNEL COMMAND LINE  (modprobe.blacklist= / module_blacklist= / pci-stub.ids=)"
sub "running kernel"
note "$(cat /proc/cmdline)"
sub "/etc/kernel/cmdline  (what systemd-boot / sdboot-manage boots with)"
if [ -f /etc/kernel/cmdline ]; then
    sed 's/^/  /' /etc/kernel/cmdline
    grep -qE "$CMDKW" /etc/kernel/cmdline && note ">>> CONTAINS A BLACKLIST ARGUMENT <<<"
else
    note "(absent -- not a systemd-boot/sdboot-manage setup)"
fi
sub "/etc/default/grub  (GRUB_CMDLINE_* lines only)"
if [ -f /etc/default/grub ]; then
    grep -nE '^GRUB_CMDLINE' /etc/default/grub | sed 's/^/  /'
    grep -qE "$CMDKW" /etc/default/grub && note ">>> CONTAINS A BLACKLIST ARGUMENT <<<"
else
    note "(absent -- not a GRUB setup)"
fi
sub "GRUB drop-ins / custom scripts"
greptree /etc/default/grub.d "$CMDKW"
greptree /etc/grub.d "$CMDKW"
sub "systemd-boot loader entries"
n=0
for f in /boot/loader/entries/*.conf; do
    [ -f "$f" ] || continue
    n=$((n+1))
    printf '\n--- %s ---\n' "$f"
    grep -nE '^options' "$f" | sed 's/^/  /'
done
[ "$n" -eq 0 ] && note "(none found -- either UKI-based, GRUB-based, or /boot not mounted here)"
sub "Limine / other bootloaders"
for f in /etc/limine.conf /boot/limine.conf; do
    [ -e "$f" ] && note "found: $f"
done
sub "Built GRUB config (read-only check; regenerated on every grub-mkconfig)"
[ -f /boot/grub/grub.cfg ] && grep -nE "$CMDKW" /boot/grub/grub.cfg | head -5 | sed 's/^/  /' || note "(no /boot/grub/grub.cfg)"

# ----------------------------------------------------------------------------
hdr "5. RFKILL STATE FILE  (this literally IS 'a system file that disabled the chip')"
sub "/var/lib/systemd/rfkill/"
if [ -d /var/lib/systemd/rfkill ]; then
    for f in /var/lib/systemd/rfkill/*; do
        [ -f "$f" ] || continue
        printf '  %-58s = %s' "$(basename "$f")" "$(cat "$f" 2>/dev/null)"
        [ "$(cat "$f" 2>/dev/null)" = "1" ] && printf '   <-- BLOCKED (persisted soft-block)\n' || printf '\n'
    done
else
    note "(dir absent -- systemd never persisted a block)"
fi
note "A file here containing '1' = soft-blocked at every boot by systemd-rfkill."

# ----------------------------------------------------------------------------
hdr "6. NETWORK MANAGER LAYER  (chip works, but NM refuses to use it)"
sub "/var/lib/NetworkManager/NetworkManager.state  (WirelessEnabled=false lives here)"
[ -f /var/lib/NetworkManager/NetworkManager.state ] \
    && sed 's/^/  /' /var/lib/NetworkManager/NetworkManager.state \
    || note "(no state file)"
sub "/etc/NetworkManager/conf.d  -- FULL CONTENTS"
dumpdir /etc/NetworkManager/conf.d
sub "/etc/NetworkManager/NetworkManager.conf"
[ -f /etc/NetworkManager/NetworkManager.conf ] && sed 's/^/  /' /etc/NetworkManager/NetworkManager.conf
sub "Connections with autoconnect=false (stops connecting, does NOT disable the chip)"
greptree /etc/NetworkManager/system-connections 'autoconnect=false'

# ----------------------------------------------------------------------------
hdr "7. OTHER NETWORK STACKS"
sub "systemd-networkd  (/etc/systemd/network)"
dumpdir /etc/systemd/network
sub "iwd"
dumpdir /etc/iwd
sub "connman / netplan / ifupdown"
for d in /etc/connman /etc/netplan; do [ -d "$d" ] && dumpdir "$d"; done
[ -f /etc/network/interfaces ] && { printf '\n--- /etc/network/interfaces ---\n'; sed 's/^/    /' /etc/network/interfaces; }

# ----------------------------------------------------------------------------
hdr "8. UDEV RULES  (authorized=0, driver_override, rfkill on hotplug)"
sub "/etc/udev/rules.d  -- FULL CONTENTS"
dumpdir /etc/udev/rules.d
sub "Matches in all udev rule locations"
for d in /etc/udev/rules.d /usr/lib/udev/rules.d /run/udev/rules.d; do
    greptree "$d" 'authorized|rfkill|wlan|wifi|wireless|driver_override|NM_UNMANAGED'
done

# ----------------------------------------------------------------------------
hdr "9. DEVICE AUTHORIZATION RIGHT NOW  (authorized=0 kills the device dead)"
n=0
if have lspci; then
    for dev in $(lspci -D 2>/dev/null | grep -iE 'network|wireless' | awk '{print $1}'); do
        a="/sys/bus/pci/devices/$dev/authorized"
        if [ -f "$a" ]; then
            n=$((n+1))
            note "$dev authorized=$(cat "$a")  ($(lspci -s "$dev" 2>/dev/null | cut -d' ' -f2-))"
        fi
    done
fi
[ "$n" -eq 0 ] && note "(no PCI network device on this machine)"

# ----------------------------------------------------------------------------
hdr "10. SERVICE LAYER  (masked/disabled units, custom 'wifi-off' services)"
sub "Wifi-relevant units that are masked or disabled"
if have systemctl; then
    systemctl list-unit-files --state=masked,disabled 2>/dev/null \
        | grep -iE 'wpa_supplicant|iwd|NetworkManager|systemd-rfkill|wifi|wireless|radio' | sed 's/^/  /'
fi
sub "Custom units mentioning rfkill / nmcli / iw / ip link down"
greptree /etc/systemd/system 'rfkill|nmcli|iw dev|ip link set .* down|wifi'
sub "rfkill-block@ instances -- systemd ships 'rfkill block %I' at boot (easy to forget)"
found=0
for f in /etc/systemd/system/multi-user.target.wants/*rfkill* /etc/systemd/system/*rfkill*; do
    [ -e "$f" ] || continue
    found=1
    note "$f"
done
[ "$found" -eq 0 ] && note "(no enabled rfkill-block@/rfkill-unblock@ instances)"
note "enable/disable check:  systemctl is-enabled 'rfkill-block@all.service' 'rfkill-block@wlan0.service'"
sub "Timers / cron @reboot"
greptree /etc/cron.d '@reboot'
[ -f /etc/crontab ] && grep -nE '@reboot' /etc/crontab | sed 's/^/  /'
sub "/etc/rc.local"
[ -f /etc/rc.local ] && sed 's/^/  /' /etc/rc.local || note "(absent)"
sub "GUI autostart"
greptree /etc/xdg/autostart 'rfkill|nmcli|wifi|wireless'

# ----------------------------------------------------------------------------
hdr "11. FIRMWARE FILES  (renamed/moved firmware = driver loads but radio is dead)"
sub "Renamed/disabled firmware leftovers"
find /lib/firmware /usr/lib/firmware -maxdepth 3 \
     \( -name '*.bak' -o -name '*.disabled' -o -name '*.orig' -o -name '*.old' -o -name '*.off' \) \
     2>/dev/null | head -20 | sed 's/^/  /'
note "(nothing above = no renamed firmware)"
sub "Wi-Fi firmware present for your chip"
find /lib/firmware /usr/lib/firmware -maxdepth 2 -type f 2>/dev/null \
     | grep -iE 'iwlwifi|ath1[012]k|ath9k|rtw|rtl|mt76|brcm' 2>/dev/null | head -10 | sed 's/^/  /'

# ----------------------------------------------------------------------------
hdr "12. MOST RECENTLY TOUCHED CANDIDATE FILES  (mtime = when you did it)"
printf '  %s\n' "sorted newest first; look for a date that matches 'awhile back'"
find /etc/modprobe.d /etc/modules-load.d /etc/udev/rules.d /etc/NetworkManager \
     /etc/systemd/network /etc/systemd/system /etc/mkinitcpio.conf /etc/mkinitcpio.conf.d \
     /etc/kernel/cmdline /etc/default/grub /etc/iwd /etc/rc.local /etc/cron.d \
     -maxdepth 2 -type f -printf '%TY-%Tm-%Td %TH:%TM  %p\n' 2>/dev/null \
     | sort -r | head -25 | sed 's/^/  /'

# ----------------------------------------------------------------------------
hdr "13. DID A PACKAGED CONFIG GET EDITED?  (pacman remembers)"
note "Run these to see which shipped config files you changed:"
note "  pacman -Qii networkmanager | grep -A20 'Backup Files'"
note "  pacman -Qii systemd wpa_supplicant iwd | grep -A20 'Backup Files'"
note "  pacman -Qkk networkmanager systemd 2>&1 | grep -v ' 0 altered'"
note "'MODIFIED' next to a path = you edited that file. This is the fastest answer."

# ============================================================================
#  SECOND WAVE -- the places people forget
# ============================================================================
hdr "14. SECOND WAVE  (sysfs writers, dispatchers, drop-ins, policy)"
sub "/etc/tmpfiles.d  -- 'w' lines write sysfs at every boot (authorized=0, remove=1)"
dumpdir /etc/tmpfiles.d
greptree /usr/lib/tmpfiles.d 'authorized|/remove|rfkill|wlan|wifi'
sub "NetworkManager dispatcher scripts  (fire on every interface event)"
dumpdir /etc/NetworkManager/dispatcher.d
for d in /etc/NetworkManager/dispatcher.d/pre-up.d /etc/NetworkManager/dispatcher.d/no-up.d; do
    [ -d "$d" ] && dumpdir "$d"
done
sub "systemd drop-ins on network units"
for d in /etc/systemd/system/NetworkManager.service.d \
         /etc/systemd/system/wpa_supplicant@.service.d \
         /etc/systemd/system/wpa_supplicant.service.d \
         /etc/systemd/system/iwd.service.d \
         /etc/systemd/system/systemd-rfkill.service.d; do
    [ -d "$d" ] && dumpdir "$d"
done
greptree /etc/systemd/system 'ExecStartPre|ConditionPathExists|ConditionCapability'
sub "Unit presets that could disable radio services"
dumpdir /etc/systemd/system-preset
sub "Policy / permission layers"
dumpdir /etc/polkit-1/rules.d
greptree /etc/apparmor.d 'rfkill|netlink|network'
greptree /etc/sysctl.d 'modules_disabled'
[ -f /etc/sysctl.conf ] && grep -nE 'modules_disabled' /etc/sysctl.conf | sed 's/^/  /'
sub "Interface renaming (NM profiles no longer match the device)"
greptree /etc/systemd/network 'Name=|Unmanaged='
sub "Blacklisted *companion* modules -- driver loads, but no wlan iface ever appears"
if have modprobe; then
    modprobe --showconfig 2>/dev/null | grep -E '^blacklist (iwlmvm|iwldvm|mac80211|cfg80211|iwlwifi)' | sed 's/^/  /' \
        || note "(none)"
fi
sub "USB authorization / usbguard (only matters for a USB dongle)"
greptree /etc/udev/rules.d 'authorized_default'
[ -f /etc/usbguard/rules.conf ] && note "usbguard rules present: /etc/usbguard/rules.conf" || note "(no usbguard rules)"

# ----------------------------------------------------------------------------
hdr "15. SEARCH BY CONTENT  (finds files nobody thought to list)"
note "Greps every config tree for the driver names actually on this box."
DRIVERS=$( { lspci -nnk 2>/dev/null | grep -E 'Kernel (driver in use|modules):'; lsmod 2>/dev/null; } \
    | grep -oiE 'iwlwifi|iwlmvm|iwldvm|ath[0-9]+k|ath1[012]k|rtw[0-9]+|rtl[0-9]+|mt7[0-9]+|mt76[0-9a-z]*|brcmfmac|brcmsmac|bcma' \
    | sort -u | paste -sd'|' - )
[ -z "$DRIVERS" ] && DRIVERS='iwlwifi|iwlmvm|ath[0-9]+k|ath1[012]k|rtw[0-9]+|rtl[0-9]+|mt7[0-9]+|mt76[0-9a-z]*|brcmfmac|brcmsmac'
note "patterns: $DRIVERS"
echo
grep -rIlE "$DRIVERS|cfg80211|mac80211|rfkill" \
    --exclude-dir=ssl --exclude-dir=ca-certificates --exclude-dir=gnupg --exclude-dir=fonts \
    --exclude-dir=hwdb.d --exclude=group --exclude=group- --exclude=passwd --exclude=passwd- \
    --exclude=shadow --exclude=gshadow \
    /etc /usr/lib/systemd /usr/lib/udev /usr/lib/tmpfiles.d /usr/lib/modprobe.d /usr/lib/NetworkManager \
    2>/dev/null | sed 's/^/  /'
note "(any path above is a file that mentions your wifi driver -- read it)"

# ----------------------------------------------------------------------------
hdr "16. DIFF THE SYSTEM AGAINST THE PAST  (definitive, beats guessing)"
note "Your root is btrfs (rootflags=subvol=/@), so this is likely available:"
note "  sudo snapper -c root list"
note "  sudo snapper -c root diff <older>..<newer>     # what changed, exactly"
note "  sudo snapper -c root status <older>..<newer>"
note ""
note "No snapper?"
note "  sudo btrfs subvolume list /            # find @snapshots"
note "  sudo mount -o subvolid=5 /dev/<esp-part> /mnt/top && ls /mnt/top/@snapshots"
note "  diff -r /mnt/top/@snapshots/<id>/etc /etc"
note ""
note "Timeshift:   sudo timeshift --list   then browse /timeshift/snapshots/"
note "etckeeper:   sudo git -C /etc log --oneline --stat"
note ""
note "Journal: which boot did it break in?"
note "  journalctl --list-boots"
note "  journalctl -k --no-pager | grep -iE 'iwlwifi|ath11k|mt79|rtw|cfg80211' | tail -40"
note "  ^ the last driver line before it goes silent usually says why"

# ----------------------------------------------------------------------------
hdr "TL;DR -- where to look first, in order"
cat <<'EOF'
  First wave
  1. /etc/modprobe.d/*.conf ................. blacklist / install <mod> /bin/false
  2. /var/lib/systemd/rfkill/*  ............. file containing "1" = soft-blocked every boot
  3. /etc/kernel/cmdline or /etc/default/grub  module_blacklist= / modprobe.blacklist=
  4. /etc/NetworkManager/conf.d/*.conf ...... unmanaged-devices= or managed=false
  5. /etc/udev/rules.d/*.rules .............. ATTR{authorized}="0" or rfkill RUN+=
  6. /etc/mkinitcpio.conf MODULES=(!mod) .... module excluded from initramfs
  7. /var/lib/NetworkManager/NetworkManager.state ... WirelessEnabled=false
  8. /usr/lib/firmware/ ..................... renamed .ucode/.bin files
  9. /etc/systemd/system/*.service .......... custom "wifi-off" unit, or masked wpa_supplicant@
 10. /etc/rc.local, /etc/cron.d/* ........... rfkill block / nmcli radio wifi off at boot

  Second wave
 11. /etc/tmpfiles.d/*.conf ................ w-line writing sysfs authorized/remove
 12. /etc/NetworkManager/dispatcher.d/ ..... script doing rfkill block on interface events
 13. /etc/systemd/system/*.service.d/*.conf . drop-in on NetworkManager/wpa_supplicant
 14. blacklist of iwlmvm/mac80211/cfg80211 .. driver loads, no wlan iface appears
 15. /etc/polkit-1/rules.d, /dev/rfkill perms  chip fine, unblocking not authorized
 16. /etc/systemd/network/*.link ............ interface renamed out from under NM

  When all else fails: section 16 -- diff a btrfs snapshot from before the change.
EOF
echo
echo "Audit complete. Nothing was changed."
