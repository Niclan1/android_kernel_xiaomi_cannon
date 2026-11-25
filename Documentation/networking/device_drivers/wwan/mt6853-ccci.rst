.. SPDX-License-Identifier: GPL-2.0-only

MT6853 CCCI modem bring-up
=========================

Clock contract
--------------

The modem runtime feature ``MISC_INFO_RTC_32K_LESS`` (feature ID 6) must
reflect the board's RTC oscillator configuration.  A working RTC and the
absence of a device-tree "32k-less" property do not establish that an
external 32.768 kHz crystal is fitted.

The vendor MT6853 implementation of ``crystal_exist_status()`` reads
``RTC_SPAR0[6]`` through ``hal_rtc_get_spare_register(RTC_32K_LESS)``.
Despite the spare field's name, a set bit means crystal present; a clear
bit means 32k-less operation.  On MT6359 this is PMIC register 0x5b8.
The CCCI port reads the same bit through the existing PMIC regmap and
leaves the RTC and all clock-control registers unchanged.  If the PMIC
cannot be read, it retains the previous non-32k-less advertisement and
logs the failure rather than guessing an oscillator configuration.

Evidence and limits
-------------------

The cannong c3y #61 boot captured on 2026-11-10 had the following evidence:

* The AP runtime-data dump at offset 0x60 begins ``00000106 00000000``:
  feature 6 is NOT_SUPPORT, with no payload.  The old compatibility stub
  always returned true, unconditionally disabling 32k-less support.
* The vendor 4.14 sources determine this feature from the RTC spare bit,
  not device tree.  Relevant sources are ``rtc/mtk_rtc_common.c`` and
  ``rtc/mt6358/mtk_rtc_hal.c`` under ``drivers/misc/mediatek/`` and the
  MT6853 ``mach/upmu_hw.h`` register definitions.
* At roughly 200 seconds, DPMAIF_AP (GIC interrupt 241) had zero interrupts
  while CCIF_AP_DATA was active.  The IRQ was registered as level-triggered,
  wake-enabled, with zero unhandled interrupts.  No uplink-triggered DPMAIF
  diagnostic had fired in that capture.  A zero DPMAIF count before data
  attachment alone does not demonstrate a broken RX IRQ or refill path.

The RTC stub is a confirmed porting error.  Whether the physical board has
the spare bit clear, and whether correcting negotiation resolves the later
``emac_slp.c:1116`` assert, require an owner-controlled device test.  Neither
an object build nor successful Image.gz linking establishes modem recovery.
No RC-channel override, DVFSRC setting, DMA layout or modem firmware is
changed by this fix.

Device test
-----------

Keep the original modem firmware, NVRAM and calibration unchanged.  Confirm
the booted kernel contains the patch before interpreting logs.  Capture:

* ``mtk-ccci: RTC_SPAR0=``: ``crystal=0 32k-less=1`` means 32k-less support
  should be negotiated; ``crystal=1 32k-less=0`` preserves the old result.
  ``cannot read RTC crystal status`` means the test is inconclusive.
* ``/proc/ccci_dump``: runtime feature 6 must agree with the reported spare
  bit and modem feature negotiation.  With 32k-less negotiated, its header
  becomes ``00000206 00000010``, followed by the 16-byte feature payload.
* ``mtk-ccci: dpmaif first DL packets:`` and the DPMAIF_AP line in
  ``/proc/interrupts``: require continued RX/TX progress after cellular data
  attaches, not merely one packet.  ``UL without DL for 20s`` indicates a
  remaining transport or modem-side problem.
* Radio logcat: monitor ``+CEREG:``, ``+CGREG:``, ``SETUP_DATA_CALL``,
  ``DATA_CALL_LIST_CHANGED`` and ``DataCallResponse`` for stable attachment
  and a usable data call; allow at least 20 minutes plus idle/wake cycles.
* Dmesg/logcat must remain free of ``emac_slp.c``, ``MD exception``,
  ``MD_FORCE_ASSERT_BY_AP_Q0_BLOCKED`` and ``rx ERR_STOP``.  Test sustained
  downlink/uplink, a real IMS call and 5G separately before claiming them
  fixed.

HS1 event context
-----------------

Runtime feature preparation can sleep: the MT6359 RTC spare-bit read uses
regmap and the mainline PMIC wrapper's sleepable polling.  The vendor FSM
held ``event_lock`` with interrupts disabled over preparation, which was
compatible with its atomic PMIC accessor but not with the ported reader.
The returned cycle7 slot1 log records this exact path at 11.820903 seconds::

  BUG: scheduling while atomic: ccci_fsm1/101/0x00000002
  usleep_range_state -> pwrap_read16 -> regmap_read
    -> crystal_exist_status -> ccci_md_prepare_runtime_data
    -> fsm_main_thread

The FSM detaches HS1 while holding the queue lock, then releases the lock
before processing the privately owned event.  It frees the event after
preparation and takes the lock again after sending runtime data.  Producers
can append HS2 or exception events during the PMIC read without losing or
freeing the in-progress HS1 payload.  No RTC value is cached or overridden.

The same capture later warns about IRQ 27, not the DPMAIF SPI.  Its short
12.104-second endpoint does not itself establish a fatal modem exception
or the cause of the eventual recovery entry.  Retest for absence of the
atomic-scheduling report and both IRQ warnings, then require HS2 and the
modem/data checks above.  Do not hide warnings or synthesize interrupts.

Isolated build input
--------------------

``cannong_defconfig`` embeds ``firmware/novatek_ts_fw.bin`` through
``CONFIG_EXTRA_FIRMWARE``.  This ignored binary must also be present in a
new worktree; otherwise the firmware-loader build fails with "No rule to
make target" before reaching CCCI.  Use the existing board firmware input,
not an empty replacement or a configuration change that drops the firmware.
Use a separate output directory and retain the board build's toolchain and
flags when checking the modem objects and Image.gz link.
