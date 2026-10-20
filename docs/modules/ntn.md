# NTN module

The NTN module adds an Iridium non-terrestrial network (NTN) fallback to the Asset Tracker Template. When no terrestrial network (TN) is available, the module suspends the Network module, switches the modem to Iridium NTN (TDD NB-IoT), and periodically sends the device's GNSS location to nRF Cloud over satellite. It returns to the terrestrial network on a configurable interval.

The module is disabled by default and does not change the behavior of the template unless you enable it with `overlay-ntn.conf`.

The same Deutsche Telekom SIM card and APN are used for the terrestrial network (LTE-M/NB-IoT) and for Iridium NTN.

## Requirements

- nRF9151 with modem firmware supporting Iridium NTN.
- A Deutsche Telekom SIM card provisioned for Iridium NTN.
- An antenna setup suitable for both GNSS and NTN.

## Building

```shell
west build -p -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE="overlay-ntn.conf"
```

## How it works

The modem is configured with two cellular profiles at boot: profile 0 for the terrestrial network and profile 1 for NTN. Both use the physical SIM card. With two profiles, the modem can be put into offline mode while keeping its registration context (`LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG`), which is how the module switches between TN, GNSS, and NTN.

1. **Terrestrial network.** The template runs as usual. If the Network module does not connect within `CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS`, or reports `NETWORK_SEARCH_DONE` (with `CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE`), the module falls back to NTN.
1. **Suspend.** The module sends `NETWORK_SUSPEND`. The Network module takes the modem offline, ignores link events, and reports `NETWORK_DISCONNECTED` followed by `NETWORK_SUSPENDED`. The Cloud module pauses the nRF Cloud CoAP session, which saves the DTLS Connection ID session in the modem.
1. **NTN idle.** Between uplinks, the modem is left in GNSS-only mode, so the Location module can get fixes. The module caches the latest GNSS location from `LOCATION_GNSS_DATA`.
1. **NTN uplink.** Every `CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS`, the module provides the location to the modem with `ntn_location_set()`, activates NTN, and waits for registration and PDN connectivity. It then resumes the CoAP session, sends the location to nRF Cloud as a confirmable message, pauses the session again, and takes the modem offline. If the location is older than `CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS`, the module triggers a new GNSS search first.
1. **Return to TN.** Every `CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES`, the module restores the terrestrial system mode and sends `NETWORK_RESUME`. The Network module searches for a network as usual, and the Cloud module resumes the CoAP session when connected.

GNSS and NTN cannot be active at the same time, so the module time-multiplexes them.

## Messages

The NTN module communicates through the zbus channel `ntn_chan`. The messages are defined in `ntn.h`.

### Input messages

- **NTN_FALLBACK_START_REQUEST**: Fall back to NTN immediately.
- **NTN_FALLBACK_STOP_REQUEST**: Stop using NTN and return to the terrestrial network.
- **NTN_UPLINK_REQUEST**: Send the location over NTN as soon as possible.
- **NTN_LOCATION_SET**: Set the location used for NTN, for example when GNSS is not available. The location is carried in `struct ntn_location`.

### Output messages

- **NTN_FALLBACK_STARTED**: The module has suspended the Network module and is using NTN.
- **NTN_FALLBACK_STOPPED**: The module has returned control to the Network module.
- **NTN_UPLINK_SENT**: The location was sent to nRF Cloud over NTN.
- **NTN_UPLINK_FAILED**: Sending the location over NTN failed.

## Changes to other modules

The NTN module reuses the rest of the template. It needs two small generic additions, which are only active when selected by `CONFIG_APP_NTN`:

- **Network module**: `NETWORK_SUSPEND` and `NETWORK_RESUME` input messages, the `NETWORK_SUSPENDED` output message, and a suspended state (`CONFIG_APP_NETWORK_SUSPEND_KEEP_REG`).
- **Cloud module**: pauses the nRF Cloud CoAP session when the network is lost and resumes it when reconnected, instead of doing a new DTLS handshake (`CONFIG_APP_CLOUD_SESSION_PAUSE`).

## Shell commands

With `CONFIG_APP_NTN_SHELL`, the following commands are available:

- `att_ntn start`: Fall back to NTN now.
- `att_ntn stop`: Return to the terrestrial network.
- `att_ntn send`: Send the location over NTN now.
- `att_ntn location <lat> <lon> <alt_m> [accuracy_m]`: Set the location manually.

## Configuration

- **CONFIG_APP_NTN**: Enable the module.
- **CONFIG_APP_NTN_LOG_LEVEL_DBG** / **CONFIG_NTN_LOG_LEVEL_DBG** / **CONFIG_LTE_LINK_CONTROL_LOG_LEVEL_DBG**: Debug logging for the NTN module (`ntn_module`), modem NTN library (`ntn`), and LTE Link Control (`lte_lc`). Set in `overlay-ntn.conf`.
- **CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS**: Time without a terrestrial connection before falling back to NTN.
- **CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE**: Fall back to NTN as soon as the modem has completed a full terrestrial network search.
- **CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES**: How often to try the terrestrial network again while using NTN.
- **CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS**: Location uplink interval over NTN.
- **CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS**: Maximum age of the location given to the modem. Set to 0 for no limit.
- **CONFIG_APP_NTN_CONNECTION_TIMEOUT_SECONDS**: Time allowed to register and get PDN connectivity over NTN.
- **CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS**: Time allowed after registration to get PDN connectivity.
- **CONFIG_APP_NTN_BANDLOCK_ENABLE** and **CONFIG_APP_NTN_BANDLOCK**: Lock NTN to a single band. Enabled by default with the Iridium band 249.
- **CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE** and **CONFIG_APP_NTN_CHANNEL_SELECT**: Restrict NTN to a single channel (EARFCN). Enabled by default with channel 196702.
- **CONFIG_APP_NTN_CLOUD_FULL_CONNECT**: Allow a full DTLS handshake over NTN if the paused session cannot be resumed.

## Limitations

- Each NTN uplink sends the GNSS location and refreshes the enabled device shadow info sections (`networkInfo`, `simInfo`, `connectionInfo`, and others selected under **Send shadow info sections** in Kconfig) via `nrf_cloud_coap_shadow_configured_info_update()`, matching the sections sent on initial CoAP connect. Environmental and other sensor data are still buffered by the Storage module and sent when the terrestrial network is available again.
- A location sent over NTN can also appear later in buffered storage data flushed over TN.
- Without a terrestrial network, there is no A-GNSS assistance. The first GNSS fix can take several minutes.
