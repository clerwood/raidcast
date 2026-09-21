# Locking a peer down to RaidCast only

Joining someone to your tailnet, by default, lets their machine reach **every
service on yours**. This is how to reduce that to one UDP port.

Two independent layers, and you want both:

| Layer | Stops | Fails if |
|---|---|---|
| Tailscale ACL | Packets ever reaching your machine | You mistype the policy, or forget it after a reinstall |
| Windows Firewall | Anything that got through anyway | You change the Tailscale interface's network profile |

RaidCast's own `whois` allowlist is a third layer, but it only governs who may
*watch the stream* — it does nothing about the rest of your machine.

---

## What a tailnet peer can actually reach

Check before you harden, so you know what you fixed. Run on the host:

```powershell
$ts = 'C:\Program Files\Tailscale\tailscale.exe'
$ip = (& $ts ip -4).Trim()
Get-NetTCPConnection -State Listen |
  Where-Object { $_.LocalAddress -in '0.0.0.0','::',$ip } |
  Select-Object -ExpandProperty LocalPort -Unique | Sort-Object
```

Anything on that list is reachable from every peer on your tailnet. On a stock
Windows 11 desktop it typically includes:

- **135 / 139 / 445** — RPC endpoint mapper and SMB file sharing. The one that
  actually matters: SMB is on by default and exposes your shares and,
  historically, the widest attack surface on Windows.
- **3389** — Remote Desktop, if you have ever enabled it.
- **5985 / 5986** — WinRM, i.e. remote PowerShell.
- **22** — OpenSSH Server, if you installed that optional feature.

**A remote shell via Tailscale SSH is not one of the risks.** Tailscale SSH needs
`RunSSH` enabled per-node *and* Windows is not supported as a Tailscale SSH
server. Verify with `tailscale debug prefs` — you want `"RunSSH": false`. The
shell risk on Windows comes from OpenSSH Server, RDP and WinRM instead, which is
what the steps below close.

---

## Host setup

### 1. Prefer sharing one device over inviting a user

Admin console → **Machines** → your host → **Share**. They accept the link into
*their own* tailnet and can reach that one device. They never become a member of
yours, so they cannot enumerate it.

Invite them as a user (Users → Invite external users) only if they need to be a
regular fixture — and set the ACL **before** they accept.

### 2. Restrict them to RaidCast's port in the ACL

Admin console → **Access controls**. RaidCast uses exactly one UDP port and no
dynamic ports, so the rule is narrow:

```jsonc
{
  "tagOwners": {
    "tag:raidcast-host": ["autogroup:admin"],
  },

  "acls": [
    // Keep your own devices reaching each other. Adding an "acls" block REPLACES
    // the default allow-all policy, so without this you cut yourself off.
    //
    // Note this does NOT cover the host once it is tagged: tagging removes user
    // ownership, so autogroup:self no longer includes it. Hence the next rule.
    { "action": "accept", "src": ["autogroup:member"], "dst": ["autogroup:self:*"] },

    // Your own devices reaching the tagged host.
    { "action": "accept", "src": ["autogroup:member"], "dst": ["tag:raidcast-host:*"] },

    // The viewer reaches RaidCast on the host, and nothing else anywhere.
    {
      "action": "accept",
      "src":    ["raidleader@example.com"],
      "proto":  "udp",
      "dst":    ["tag:raidcast-host:41800"],
    },

    // ...and the stream flows back. This rule is NOT optional: SRT is UDP, the
    // host answers from 41800 to whatever ephemeral port the viewer opened, and
    // there is no handshake for the packet filter to anchor return state on. The
    // default allow-all policy covered this implicitly; an "acls" block does not.
    // Omit it and the viewer's connection times out while the forward direction
    // looks perfectly configured.
    {
      "action": "accept",
      "src":    ["tag:raidcast-host"],
      "proto":  "udp",
      "dst":    ["raidleader@example.com:*"],
    },
  ],

  // Saved policies are validated against these, so a future edit that breaks the
  // return path fails in the console instead of on a raid night.
  "tests": [
    {
      "src":    "raidleader@example.com",
      "proto":  "udp",
      "accept": ["tag:raidcast-host:41800"],
    },
  ],

  // No Tailscale SSH rules at all.
  "ssh": [],
}
```

Then tag the host: **Machines** → ⋯ → **Edit ACL tags** → `tag:raidcast-host`.
Tagging rather than naming the device means the rule survives you renaming or
reinstalling the machine.

Four things that will bite you:

1. **You need the return rule.** An earlier version of this document omitted it
   and the viewer timed out, with the forward direction verifiably correct — the
   host's compiled filter showed `proto=17` from the viewer's node to port 41800,
   and `tailscale ping` reported a direct 48 ms path. ACLs are enforced on the
   *receiving* node, so permitting viewer→host says nothing about host→viewer.
2. **`"proto": "udp"` is mandatory.** SRT is UDP. A TCP-only rule blocks
   everything silently and looks exactly like a broken stream.
3. **Adding `acls` replaces the default allow-all**, and a tagged device is not
   covered by `autogroup:self` — tagging removes user ownership. Without the
   `tag:raidcast-host:*` rule you lock *yourself* out of your own host.
4. **Forwarding UDP 41641 on your router is unrelated.** That is NAT traversal
   between the Tailscale daemons. ACLs govern traffic *inside* the tunnel. You
   may need both.

## Diagnosing a timeout

Work outwards; each step rules out a layer:

```powershell
# 1. Is the tag actually on this machine? Defining tagOwners does not apply it.
tailscale status --json | ConvertFrom-Json | % { $_.Self.Tags }

# 2. Is there a path at all, and is it direct? (This bypasses ACLs.)
tailscale ping <viewer-ip>

# 3. What is the host actually enforcing? Look for proto=17 and port 41800.
tailscale debug netmap | ConvertFrom-Json | % { $_.PacketFilter }
```

If all three look right and it still times out, the return path is the remaining
candidate — which is what rule 4 above exists for.

Use the admin console's editor to save — it validates the policy and flags
unreachable rules.

### 3. Close the rest at the Windows Firewall

Defense in depth, and it does not depend on the ACL being right. The Tailscale
adapter is `Tailscale` and Windows classifies it as a **Private** network, which
is why the default File and Printer Sharing rules let SMB through.

Run as Administrator:

```powershell
# Let RaidCast in. Windows blocks unsolicited inbound by default, so this is needed.
New-NetFirewallRule -DisplayName "RaidCast (UDP 41800 in)" `
  -Direction Inbound -Action Allow -Protocol UDP -LocalPort 41800 `
  -InterfaceAlias "Tailscale"

# Close SMB, RPC and NetBIOS on the tunnel only. Block rules take precedence over
# allow rules in Windows Firewall, so these override File and Printer Sharing
# without disabling it on your real LAN.
New-NetFirewallRule -DisplayName "RaidCast: block SMB/RPC over Tailscale" `
  -Direction Inbound -Action Block -Protocol TCP -LocalPort 135,139,445 `
  -InterfaceAlias "Tailscale"

New-NetFirewallRule -DisplayName "RaidCast: block NetBIOS over Tailscale" `
  -Direction Inbound -Action Block -Protocol UDP -LocalPort 137,138 `
  -InterfaceAlias "Tailscale"

# Remote access paths, blocked on the tunnel whether or not they are running today.
New-NetFirewallRule -DisplayName "RaidCast: block remote access over Tailscale" `
  -Direction Inbound -Action Block -Protocol TCP -LocalPort 22,3389,5985,5986 `
  -InterfaceAlias "Tailscale"
```

To undo any of it:

```powershell
Get-NetFirewallRule -DisplayName "RaidCast*" | Remove-NetFirewallRule
```

### 4. Confirm the services you do not want are actually off

```powershell
foreach ($n in 'sshd','TermService','WinRM') {
  Get-Service $n -ErrorAction SilentlyContinue |
    Select-Object Name, Status, StartType
}
```

`Stopped` / `Disabled` is what you want. Anything `Running` is reachable unless
step 3 blocked it.

### 5. Re-run the check from the top of this document

The ports list should now be your RaidCast port and little else.

---

## Viewer setup

Much shorter, because **the viewer never accepts an inbound connection** — it
dials out to the host.

1. Install Tailscale and sign in.
2. Accept the device share or tailnet invite.
3. Optionally, block all inbound to your own machine over the tailnet:

   ```
   tailscale set --shields-up=true
   ```

   The viewer keeps working, because outbound connections are unaffected. Do
   **not** do this on the host — it would block RaidCast's listener.

4. Install RaidCast, open the viewer, pick the host from the list.

You do not need to forward any ports.

---

## Design note: shields-up and which end listens

`--shields-up` is the strongest single control Tailscale offers a node: it refuses
all inbound tailnet connections, so no ACL mistake can expose anything. The viewer
can use it; the host cannot, because RaidCast makes the host the SRT listener.

If the roles were reversed — host dials out to a listening viewer — the host could
run shields-up and expose **nothing at all**, and the hardening above would mostly
become unnecessary. That is a real trade-off against the current design, which
chose host-listens so the viewer stays stateless and "waiting for viewer" is the
natural host state. Recorded here rather than acted on.
