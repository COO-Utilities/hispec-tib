# Networking Configuration Plan

## Scope
Simple Zephyr-first networking helper for IPv4 Ethernet that:
- comes up without blocking app startup,
- supports field deployment variance (DHCP or static),
- preserves a known fallback service IP for direct laptop crossover recovery,
- stays separate from command parsing/dispatch logic.
- Docs: https://docs.zephyrproject.org/latest/connectivity/networking/api/net_config.html

## Capability Model (Kconfig)
Use Zephyr Kconfig as the source of truth for capability presence:
- DHCP capability: `CONFIG_NET_DHCPV4`
- DNS capability: `CONFIG_DNS_RESOLVER`
- NTP capability: `CONFIG_SNTP`

No duplicate helper Kconfig flags should mirror those capabilities.

## Configuration Layers and Precedence
1. Runtime persisted profile (settings): primary operator-configured behavior.
2. Compile-time static IPv4 defaults (`CONFIG_NET_CONFIG_MY_IPV4_*`): baseline defaults used when no persisted override exists.
3. Fallback service profile (`192.168.88.2/24`, gw `0.0.0.0` by default): recovery path for direct laptop connection.

IP selection order at apply-time:
1. DHCP (if enabled and `try_dhcp_first` is true) with bounded timeout.
2. Runtime static profile.
3. Fallback static profile.
4. DHCP retry last (if configured static-first mode is requested).

## DNS / NTP Handling
- Keep DNS and NTP addresses as configuration data in the profile.
- If DNS/NTP capability is not compiled in, treat incoming DNS/NTP settings as unsupported (not fatal to overall IP apply).
- Command layer reports unsupported fields clearly; network helper remains transport/bootstrap focused.

## Boot and Reconfigure Behavior
- Boot should not block waiting for network readiness, CONFIG_NET_CONFIG_AUTO_INIT can thus not be used. 
- Serial and local command pathways start immediately.
- Network helper brings interface up, applies config, and monitors link events.
- MQTT loop tolerates disconnect/reconnect and resumes when network is available.
- Runtime network reconfigure is helper API-level only; command-trigger wiring remains in app command/control flow.

## Library Boundary
Network helper responsibilities:
- interface bring-up/reconnect monitoring via Zephyer API
- IPv4 profile apply logic (DHCP/static/fallback),
- feature introspection (`dhcp/dns/ntp` compiled-in status),
- active IPv4 status reporting.

Out of scope for helper:
- command parsing,
- settings storage policy,
- MQTT command semantics.
