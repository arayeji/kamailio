# Kamailio - Proxy-CSCF Example Configuration File

Project Website:

  * https://www.kamailio.org

## Rx / VoLTE bearer control

Per-call **Diameter Rx** (AAR at media authorization, STR at teardown) requires:

- `#!define WITH_RX` in `pcscf.cfg`
- Modules: `cdp`, `cdp_avp`, `ims_qos`
- Diameter peer in `pcscf.xml` → Open5GS **PCRF** (App-Id `16777236`)
- `modparam("ims_qos", "rx_dest_realm", ...)` matching the PCRF realm

This fork adds **`Rx_STR_dialog()`** and module-level teardown so every AAR gets a
matching STR when the call ends (fixes QCI 1 bearer leaks). See
[../README.md](../README.md#2-rx-bearer-teardown-fix-ims_qos).

## Database Structure

The necessary Database files for the Proxy-CSCF can be found in the utils/kamctl/mysql/ folder.

The following tables (or files) are required:

  * ims_dialog-create.sql
  * ims_usrloc_pcscf-create.sql
  * presence-create.sql
  * standard-create.sql
