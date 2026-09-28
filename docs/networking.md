# Networking: reaching the PC over LTE

The phone always opens the connection (it sends HELLO to the PC), so **Fido's carrier-grade
NAT on the phone side never matters**. The PC replies to whatever address the authenticated
packets come from, and it follows the phone if its LTE IP changes. The only requirement is
that the PC has **one UDP port reachable from the internet** (default 47000).

Work down this list and stop at the first option that works. You only need a VPS if nothing
else does.

## 1. Check whether your home internet has a public IPv4 (usual case)

1. Open your router's admin page and find its **WAN / Internet IPv4 address**.
2. On the PC, open https://ifconfig.me (or run `curl -4 ifconfig.me`).
3. If the two addresses **match**, and the WAN address is **not** in `100.64.0.0 – 100.127.255.255`,
   you have a public IP. Continue below.
   If they differ, or the WAN address is `100.64.x.x`–`100.127.x.x`, your ISP uses CGNAT.
   Skip to step 2.

**Set up the forward:**

* Give the PC a fixed LAN address (DHCP reservation in the router).
* Router: port forward **UDP 47000** to that LAN address.
* Windows Firewall (run in an admin terminal):
  `netsh advfirewall firewall add rule name="FPV Truck" dir=in action=allow protocol=UDP localport=47000`
* Home IPs change occasionally, so use a free dynamic DNS name such as
  [DuckDNS](https://www.duckdns.org) and enter that name as the phone's "PC address".
* Test: turn WiFi **off** on the phone and press START. The OSD should show the phone's
  address in the top right, and `RTT` should appear.

## 2. IPv6

Rogers/Fido LTE gives phones IPv6. If your home ISP gives the PC a global IPv6 address, no
NAT is involved. `fpv-pc` listens on IPv4 and IPv6 at once.

* Find the PC's address: `ipconfig` → "IPv6 Address" (starts with 2xxx:, not fe80:).
* Allow inbound UDP 47000 to that address in the router's **IPv6 firewall**, plus the
  Windows rule above.
* Enter the IPv6 address (or a DDNS name with an AAAA record) on the phone.

## 3. Tailscale (free, no server of your own)

Install Tailscale on the PC and the phone, and sign both into the same account. Use the PC's
Tailscale address (`100.x.y.z`) as the phone's "PC address". Tailscale punches through most
NATs and gives a direct path. Check with `tailscale ping <phone>` on the PC: "via DERP" means
it is relayed and slower. Datagrams are sized to fit Tailscale's 1280-byte MTU. Encryption adds
well under a millisecond.

## 4. VPS relay (last resort)

Rent a small VPS in Toronto or Montreal and forward UDP 47000 from it to the PC over a
WireGuard tunnel that the PC opens outbound. This adds about 5–15 ms. It isn't needed unless
options 1–3 all fail.

## Force cellular

With **"Force cellular"** ticked, the app keeps its traffic on LTE even while the phone is on
WiFi. That's handy for testing the real path at home. Otherwise the phone uses whatever
network Android picks.

## Data use

Video is 4 Mbit/s at most, which is about **1.8 GB per hour** of driving. Control traffic is
negligible.
